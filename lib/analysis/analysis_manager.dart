import 'dart:isolate';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;

import '../app_log.dart';
import 'analysis_cache.dart';
import 'analysis_ffi.dart';
import 'file_hash.dart';

enum AnalysisState { idle, computingHash, generating, loading, loaded, error }

/// Localizable error stored as a typed key + positional args.
///
/// The UI resolves these via [AppLocalizations]; the manager never holds
/// translated strings.
enum AnalysisErrorKey { hashFailed, unsupported, loadFailed }

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

  AnalysisState get state => _state;
  AnalysisError? get error => _error;
  String? get generatingFileName => _generatingFileName;
  String? get loadedHash => _loadedHash;
  bool get isLoaded => _state == AnalysisState.loaded;

  /// Compute a full-file SHA-256 cache key.
  static Future<String> computeHash(String videoPath) =>
      _computeHash(videoPath);

  /// Full flow: compute hash → check cache → generate if needed → load.
  /// Returns the hash on success, null on failure.
  Future<String?> ensureAndLoad(String videoPath) async {
    final fileName = p.basename(videoPath);
    log.info('[Analysis] ensureAndLoad: videoPath=$videoPath');

    _setState(AnalysisState.computingHash);
    final String hash;
    try {
      hash = await _computeHash(videoPath);
      log.info('[Analysis] hash=$hash');
    } catch (e) {
      log.severe('[Analysis] hash failed: $e');
      _setError(AnalysisErrorKey.hashFailed, ['$e']);
      return null;
    }

    if (AnalysisCache.hasEntry(hash, videoPath: videoPath)) {
      log.info('[Analysis] cache hit for $hash, loading from cache');
      return _loadFromCache(hash, fileName, videoPath);
    }
    log.info('[Analysis] cache miss, will generate');

    _state = AnalysisState.generating;
    _generatingFileName = fileName;
    _error = null;
    notifyListeners();

    log.info(
      '[Analysis] calling FFI generateAnalysis(videoPath=$videoPath, hash=$hash)',
    );
    final ok = await Isolate.run(
      () => AnalysisFfi.generateAnalysis(videoPath, hash),
    );
    if (!ok) {
      log.severe('[Analysis] generateAnalysis returned false');
      _setError(AnalysisErrorKey.unsupported, [fileName]);
      return null;
    }
    log.info('[Analysis] generateAnalysis succeeded');

    await AnalysisCache.addEntry(hash, fileName, videoPath);
    log.info('[Analysis] index entry saved');

    return _loadFromCache(hash, fileName, videoPath);
  }

  Future<String?> _loadFromCache(String hash, String name, String path) async {
    _setState(AnalysisState.loading);
    final vbs2 = AnalysisCache.vbs2Path(hash);
    final vbi = AnalysisCache.vbiPath(hash);
    final vbt = AnalysisCache.vbtPath(hash);

    // VBS2 is optional — pass empty string if file doesn't exist
    final vbs2Arg = File(vbs2).existsSync() ? vbs2 : '';
    log.info(
      '[Analysis] loading: vbs2=${vbs2Arg.isNotEmpty ? vbs2Arg : "(skip)"}, vbi=$vbi, vbt=$vbt',
    );
    final ok = AnalysisFfi.load(vbs2Arg, vbi, vbt);
    if (!ok) {
      log.severe('[Analysis] FFI load returned false');
      _setError(AnalysisErrorKey.loadFailed, [name]);
      return null;
    }

    _loadedHash = hash;
    log.info('[Analysis] loaded successfully, hash=$hash');
    _setState(AnalysisState.loaded);
    return hash;
  }

  void unload() {
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

  static Future<String> _computeHash(String path) => computeFileSha256(path);
}
