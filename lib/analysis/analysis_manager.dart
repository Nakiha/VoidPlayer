import 'dart:isolate';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;

import '../app_log.dart';
import '../config/app_config.dart';
import 'analysis_cache.dart';
import 'analysis_ffi.dart';
import 'file_hash.dart';

enum AnalysisState { idle, computingHash, generating, loading, loaded, error }

/// Localizable error stored as a typed key + positional args.
///
/// The UI resolves these via [AppLocalizations]; the manager never holds
/// translated strings.
enum AnalysisErrorKey {
  hashFailed,
  unsupported,
  loadFailed,
  cacheLimitExceeded,
}

class AnalysisError {
  final AnalysisErrorKey key;
  final List<String> args;
  const AnalysisError(this.key, [this.args = const []]);
}

/// Dart-side state machine for the analysis generation + loading flow.
///
/// The UI (AnalysisPanel) listens to this via [ChangeNotifier] to show
/// progress / error / loaded states.
class AnalysisManager extends ChangeNotifier {
  AnalysisManager._();
  static final AnalysisManager instance = AnalysisManager._();

  AnalysisState _state = AnalysisState.idle;
  AnalysisError? _error;
  String? _generatingFileName;
  String? _loadedHash;
  final Map<String, Future<String?>> _ensureGeneratedInFlightByPath = {};
  int _stateSerial = 0;
  int _loadSerial = 0;
  int _ensureAndLoadSerial = 0;
  Future<void> _generateQueue = Future<void>.value();

  AnalysisState get state => _state;
  AnalysisError? get error => _error;
  String? get generatingFileName => _generatingFileName;
  String? get loadedHash => _loadedHash;
  bool get isLoaded => _state == AnalysisState.loaded;

  /// Compute a full-file SHA-256 cache key.
  static Future<String> computeHash(String videoPath) =>
      _computeHash(videoPath);

  /// Ensure the cache files for [videoPath] exist.
  ///
  /// Generation is deduplicated by path and is intentionally independent from
  /// the current native loaded session. Starting analysis for another file
  /// must not invalidate a generation already running for this one.
  Future<String?> ensureGenerated(String videoPath) {
    final existing = _ensureGeneratedInFlightByPath[videoPath];
    if (existing != null) return existing;

    late final Future<String?> future;
    future = _ensureGeneratedImpl(videoPath).whenComplete(() {
      if (identical(_ensureGeneratedInFlightByPath[videoPath], future)) {
        _ensureGeneratedInFlightByPath.remove(videoPath);
      }
    });
    _ensureGeneratedInFlightByPath[videoPath] = future;
    return future;
  }

  /// Full flow: compute hash → check cache → generate if needed → load.
  /// Returns the hash on success, null on failure.
  Future<String?> ensureAndLoad(String videoPath) async {
    final serial = ++_ensureAndLoadSerial;
    final hash = await ensureGenerated(videoPath);
    if (serial != _ensureAndLoadSerial) return null;
    if (hash == null) return null;
    final loaded = await loadAnalysisHash(
      hash,
      name: p.basename(videoPath),
      path: videoPath,
    );
    if (serial != _ensureAndLoadSerial) return null;
    return loaded ? hash : null;
  }

  Future<String?> _ensureGeneratedImpl(String videoPath) async {
    final fileName = p.basename(videoPath);
    final stateSerial = ++_stateSerial;
    log.info('[Analysis] ensureGenerated: videoPath=$videoPath');

    _setStateIfCurrent(stateSerial, AnalysisState.computingHash);
    final String hash;
    try {
      hash = await _computeHash(videoPath);
      log.info('[Analysis] hash=$hash');
    } catch (e) {
      log.severe('[Analysis] hash failed: $e');
      if (_isStateCurrent(stateSerial)) {
        _setError(AnalysisErrorKey.hashFailed, ['$e']);
      }
      return null;
    }

    if (AnalysisCache.hasEntry(hash, videoPath: videoPath)) {
      log.info('[Analysis] cache hit for $hash');
      await _refreshCacheEntry(hash, fileName, videoPath);
      _setStateIfCurrent(stateSerial, AnalysisState.idle);
      return hash;
    }
    log.info('[Analysis] cache miss, will generate');

    final maxCacheBytes = AppConfig.isInitialized
        ? AppConfig.instance.analysisCacheMaxBytes
        : 0;
    if (maxCacheBytes > 0) {
      final snapshot = await AnalysisCache.snapshot(maxBytes: maxCacheBytes);
      if (snapshot.isOverLimit) {
        log.warning(
          '[Analysis] cache limit reached: '
          'current=${snapshot.totalBytes}, max=$maxCacheBytes',
        );
        if (_isStateCurrent(stateSerial)) {
          _setError(AnalysisErrorKey.cacheLimitExceeded, [
            AnalysisCache.formatBytes(snapshot.totalBytes),
            AnalysisCache.formatBytes(maxCacheBytes),
          ]);
        }
        return null;
      }
    }

    if (_isStateCurrent(stateSerial)) {
      _state = AnalysisState.generating;
      _generatingFileName = fileName;
      _error = null;
      notifyListeners();
    }

    log.info(
      '[Analysis] calling FFI generateAnalysis(videoPath=$videoPath, hash=$hash)',
    );
    final bool ok;
    try {
      ok = await _generateAnalysisSerialized(videoPath, hash);
    } catch (e, stack) {
      log.severe('[Analysis] generateAnalysis threw: $e', e, stack);
      if (_isStateCurrent(stateSerial)) {
        _setError(AnalysisErrorKey.unsupported, [fileName]);
      }
      return null;
    }
    if (!ok) {
      log.severe('[Analysis] generateAnalysis returned false');
      if (_isStateCurrent(stateSerial)) {
        _setError(AnalysisErrorKey.unsupported, [fileName]);
      }
      return null;
    }
    log.info('[Analysis] generateAnalysis succeeded');

    await _refreshCacheEntry(hash, fileName, videoPath);
    log.info('[Analysis] index entry saved');

    if (maxCacheBytes > 0) {
      final snapshot = await AnalysisCache.snapshot(maxBytes: maxCacheBytes);
      if (snapshot.isOverLimit) {
        log.warning(
          '[Analysis] cache exceeded after generation: '
          'current=${snapshot.totalBytes}, max=$maxCacheBytes',
        );
      }
    }

    _setStateIfCurrent(stateSerial, AnalysisState.idle);
    return hash;
  }

  Future<bool> loadAnalysisHash(
    String hash, {
    required String name,
    required String path,
  }) async {
    final serial = ++_loadSerial;
    _setState(AnalysisState.loading);
    final analysisPath = AnalysisCache.analysisPath(hash);

    log.info('[Analysis] loading: analysis=$analysisPath');
    final ok = AnalysisFfi.load(analysisPath);
    if (!_isLoadCurrent(serial)) return false;
    if (!ok) {
      log.severe('[Analysis] FFI load returned false');
      _setError(AnalysisErrorKey.loadFailed, [name]);
      return false;
    }

    _loadedHash = hash;
    log.info('[Analysis] loaded successfully, hash=$hash');
    _setState(AnalysisState.loaded);
    return true;
  }

  void unload() {
    _ensureAndLoadSerial++;
    _stateSerial++;
    _loadSerial++;
    _ensureGeneratedInFlightByPath.clear();
    if (_state == AnalysisState.loaded) {
      AnalysisFfi.unload();
    }
    _loadedHash = null;
    _setState(AnalysisState.idle);
  }

  // ---- Internal ----

  void _setState(AnalysisState s) {
    _state = s;
    _error = null;
    _generatingFileName = null;
    notifyListeners();
  }

  void _setError(AnalysisErrorKey key, [List<String> args = const []]) {
    _state = AnalysisState.error;
    _error = AnalysisError(key, args);
    _generatingFileName = null;
    notifyListeners();
  }

  void _setStateIfCurrent(int serial, AnalysisState state) {
    if (_isStateCurrent(serial)) _setState(state);
  }

  bool _isStateCurrent(int serial) => serial == _stateSerial;
  bool _isLoadCurrent(int serial) => serial == _loadSerial;

  Future<bool> _generateAnalysisSerialized(String videoPath, String hash) {
    final previous = _generateQueue;
    final task = previous
        .catchError((_) {})
        .then(
          (_) =>
              Isolate.run(() => AnalysisFfi.generateAnalysis(videoPath, hash)),
        );
    _generateQueue = task.then<void>((_) {}, onError: (_) {});
    return task;
  }

  Future<void> _refreshCacheEntry(
    String hash,
    String fileName,
    String videoPath,
  ) async {
    try {
      await AnalysisCache.addEntry(hash, fileName, videoPath);
    } catch (e, stack) {
      log.warning('[Analysis] failed to refresh cache index: $e', e, stack);
    }
  }

  static Future<String> _computeHash(String path) => computeFileSha256(path);
}
