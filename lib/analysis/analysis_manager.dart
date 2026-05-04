import 'dart:io';
import 'dart:isolate';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;

import '../app_log.dart';
import '../config/app_config.dart';
import 'analysis_cache.dart';
import 'analysis_ffi.dart';
import 'file_hash.dart';
import 'nalu_types.dart';

enum AnalysisState { idle, computingHash, generating, loading, loaded, error }

enum AnalysisTrackStatus {
  idle,
  computingHash,
  generating,
  loading,
  cached,
  error,
}

/// Localizable error stored as a typed key + positional args.
///
/// The UI resolves these via [AppLocalizations]; the manager never holds
/// translated strings.
enum AnalysisErrorKey {
  hashFailed,
  unsupported,
  loadFailed,
  cacheLimitExceeded,
  cacheWriteIncomplete,
}

class AnalysisError {
  final AnalysisErrorKey key;
  final List<String> args;
  const AnalysisError(this.key, [this.args = const []]);
}

class AnalysisTrackGenerationStatus {
  final String path;
  final String fileName;
  final String? hash;
  final AnalysisTrackStatus status;
  final double progress;
  final AnalysisError? error;

  const AnalysisTrackGenerationStatus({
    required this.path,
    required this.fileName,
    required this.hash,
    required this.status,
    required this.progress,
    required this.error,
  });

  bool get isWorking =>
      status == AnalysisTrackStatus.computingHash ||
      status == AnalysisTrackStatus.generating ||
      status == AnalysisTrackStatus.loading;

  bool get isCached => status == AnalysisTrackStatus.cached;
  bool get isError => status == AnalysisTrackStatus.error;
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
  final Map<String, AnalysisTrackGenerationStatus> _trackStatusByPath = {};
  int _stateSerial = 0;
  int _loadSerial = 0;
  int _ensureAndLoadSerial = 0;
  Future<void> _generateQueue = Future<void>.value();

  AnalysisState get state => _state;
  AnalysisError? get error => _error;
  String? get generatingFileName => _generatingFileName;
  String? get loadedHash => _loadedHash;
  bool get isLoaded => _state == AnalysisState.loaded;

  AnalysisTrackGenerationStatus? statusForPath(String path) =>
      _trackStatusByPath[path];

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

  /// Full flow: find cached hash or compute hash → generate if needed → load.
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
    _setTrackStatus(
      videoPath,
      fileName: fileName,
      status: AnalysisTrackStatus.computingHash,
      progress: 0,
    );
    final indexedHash = await AnalysisCache.findHashForUnchangedVideo(
      videoPath,
    );
    if (indexedHash != null && _hasUsableCacheEntry(indexedHash, videoPath)) {
      log.info('[Analysis] metadata cache hit for $indexedHash');
      await _refreshCacheEntry(indexedHash, fileName, videoPath);
      await AnalysisCache.touchEntry(indexedHash);
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: indexedHash,
        status: AnalysisTrackStatus.cached,
        progress: 1,
      );
      _setStateIfCurrent(stateSerial, AnalysisState.idle);
      return indexedHash;
    }

    final String hash;
    try {
      hash = await _computeHash(videoPath);
      log.info('[Analysis] hash=$hash');
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: hash,
        status: AnalysisTrackStatus.computingHash,
        progress: 0,
      );
    } catch (e) {
      log.severe('[Analysis] hash failed: $e');
      final error = AnalysisError(AnalysisErrorKey.hashFailed, ['$e']);
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        status: AnalysisTrackStatus.error,
        progress: 0,
        error: error,
      );
      if (_isStateCurrent(stateSerial)) {
        _setErrorObject(error);
      }
      return null;
    }

    if (_hasUsableCacheEntry(hash, videoPath)) {
      log.info('[Analysis] cache hit for $hash');
      await _refreshCacheEntry(hash, fileName, videoPath);
      await AnalysisCache.touchEntry(hash);
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: hash,
        status: AnalysisTrackStatus.cached,
        progress: 1,
      );
      _setStateIfCurrent(stateSerial, AnalysisState.idle);
      return hash;
    }
    log.info('[Analysis] cache miss, will generate');

    final maxCacheBytes = AppConfig.isInitialized
        ? AppConfig.instance.analysisCacheMaxBytes
        : 0;
    if (maxCacheBytes > 0) {
      final pruneResult = await AnalysisCache.enforceLimit(
        maxBytes: maxCacheBytes,
        protectedHashes: {hash},
      );
      if (pruneResult.snapshot.isOverLimit) {
        log.warning(
          '[Analysis] cache limit reached: '
          'current=${pruneResult.snapshot.totalBytes}, max=$maxCacheBytes',
        );
        final error = AnalysisError(AnalysisErrorKey.cacheLimitExceeded, [
          AnalysisCache.formatBytes(pruneResult.snapshot.totalBytes),
          AnalysisCache.formatBytes(maxCacheBytes),
        ]);
        _setTrackStatus(
          videoPath,
          fileName: fileName,
          hash: hash,
          status: AnalysisTrackStatus.error,
          progress: 0,
          error: error,
        );
        if (_isStateCurrent(stateSerial)) {
          _setErrorObject(error);
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
    _setTrackStatus(
      videoPath,
      fileName: fileName,
      hash: hash,
      status: AnalysisTrackStatus.generating,
      progress: 0,
    );

    log.info(
      '[Analysis] calling FFI generateAnalysis(videoPath=$videoPath, hash=$hash)',
    );
    final bool ok;
    try {
      ok = await _generateAnalysisSerialized(videoPath, hash);
    } catch (e, stack) {
      log.severe('[Analysis] generateAnalysis threw: $e', e, stack);
      final error = await _generationFailureError(
        hash: hash,
        fileName: fileName,
        maxCacheBytes: maxCacheBytes,
      );
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: hash,
        status: AnalysisTrackStatus.error,
        progress: 0,
        error: error,
      );
      if (_isStateCurrent(stateSerial)) {
        _setErrorObject(error);
      }
      return null;
    }
    if (!ok) {
      log.severe('[Analysis] generateAnalysis returned false');
      final error = await _generationFailureError(
        hash: hash,
        fileName: fileName,
        maxCacheBytes: maxCacheBytes,
      );
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: hash,
        status: AnalysisTrackStatus.error,
        progress: 0,
        error: error,
      );
      if (_isStateCurrent(stateSerial)) {
        _setErrorObject(error);
      }
      return null;
    }
    log.info('[Analysis] generateAnalysis succeeded');

    if (!AnalysisCache.filesExist(hash)) {
      final error = await _generationFailureError(
        hash: hash,
        fileName: fileName,
        maxCacheBytes: maxCacheBytes,
        forceIncomplete: true,
      );
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: hash,
        status: AnalysisTrackStatus.error,
        progress: 0,
        error: error,
      );
      if (_isStateCurrent(stateSerial)) {
        _setErrorObject(error);
      }
      return null;
    }

    await _refreshCacheEntry(hash, fileName, videoPath);
    log.info('[Analysis] index entry saved');

    if (maxCacheBytes > 0) {
      final pruneResult = await AnalysisCache.enforceLimit(
        maxBytes: maxCacheBytes,
        protectedHashes: {hash},
      );
      if (pruneResult.snapshot.isOverLimit) {
        log.warning(
          '[Analysis] cache exceeded after generation: '
          'current=${pruneResult.snapshot.totalBytes}, max=$maxCacheBytes',
        );
      }
    }

    _setTrackStatus(
      videoPath,
      fileName: fileName,
      hash: hash,
      status: AnalysisTrackStatus.cached,
      progress: 1,
    );
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
    _setTrackStatus(
      path,
      fileName: name,
      hash: hash,
      status: AnalysisTrackStatus.loading,
      progress: 1,
    );
    final analysisPath = AnalysisCache.analysisPath(hash);

    log.info('[Analysis] loading: analysis=$analysisPath');
    final ok = AnalysisFfi.load(analysisPath);
    if (!_isLoadCurrent(serial)) return false;
    if (!ok) {
      log.severe('[Analysis] FFI load returned false');
      final error = AnalysisError(AnalysisErrorKey.loadFailed, [name]);
      _setTrackStatus(
        path,
        fileName: name,
        hash: hash,
        status: AnalysisTrackStatus.error,
        progress: 1,
        error: error,
      );
      _setErrorObject(error);
      return false;
    }

    _loadedHash = hash;
    await AnalysisCache.touchEntry(hash);
    _setTrackStatus(
      path,
      fileName: name,
      hash: hash,
      status: AnalysisTrackStatus.cached,
      progress: 1,
    );
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

  void _setErrorObject(AnalysisError error) {
    _state = AnalysisState.error;
    _error = error;
    _generatingFileName = null;
    notifyListeners();
  }

  void _setStateIfCurrent(int serial, AnalysisState state) {
    if (_isStateCurrent(serial)) _setState(state);
  }

  bool _isStateCurrent(int serial) => serial == _stateSerial;
  bool _isLoadCurrent(int serial) => serial == _loadSerial;

  bool _hasUsableCacheEntry(String hash, String videoPath) {
    if (!AnalysisCache.hasEntry(hash, videoPath: videoPath)) return false;

    AnalysisSession? session;
    try {
      session = AnalysisSession.open(AnalysisCache.analysisPath(hash));
      if (session == null || !session.isOpen) {
        log.info('[Analysis] cache stale for $hash: cannot open container');
        return false;
      }

      final summary = session.summary;
      if (summary.loaded == 0 ||
          summary.packetCount <= 0 ||
          summary.naluCount <= 0) {
        log.info(
          '[Analysis] cache stale for $hash: '
          'loaded=${summary.loaded}, packets=${summary.packetCount}, '
          'nalus=${summary.naluCount}',
        );
        return false;
      }

      final codec = analysisCodecFromValue(summary.codec);
      if (_requiresFrameData(codec) && summary.frameCount <= 0) {
        log.info(
          '[Analysis] cache stale for $hash: codec=${analysisCodecName(codec)} '
          'requires VBS4 frame data',
        );
        return false;
      }
      if (_isOlderThanFfmpegAnalyzer(hash, codec)) {
        log.info(
          '[Analysis] cache stale for $hash: codec=${analysisCodecName(codec)} '
          'was generated before current FFmpeg analyzer',
        );
        return false;
      }

      return true;
    } catch (e, stack) {
      log.warning('[Analysis] cache validation failed for $hash: $e', e, stack);
      return false;
    } finally {
      session?.close();
    }
  }

  bool _requiresFrameData(AnalysisCodec codec) => switch (codec) {
    AnalysisCodec.h264 || AnalysisCodec.hevc || AnalysisCodec.vvc => true,
    AnalysisCodec.vp9 ||
    AnalysisCodec.mpeg2 ||
    AnalysisCodec.av1 ||
    AnalysisCodec.unknown => false,
  };

  bool _isOlderThanFfmpegAnalyzer(String hash, AnalysisCodec codec) {
    if (codec != AnalysisCodec.h264 && codec != AnalysisCodec.hevc) {
      return false;
    }
    try {
      final analysisFile = File(AnalysisCache.analysisPath(hash));
      final analyzerFile = File(
        p.join(
          p.dirname(Platform.resolvedExecutable),
          'tools',
          'ffmpeg-analysis',
          'void_ffmpeg_analyzer.exe',
        ),
      );
      if (!analysisFile.existsSync() || !analyzerFile.existsSync()) {
        return false;
      }
      return analysisFile.lastModifiedSync().isBefore(
        analyzerFile.lastModifiedSync(),
      );
    } catch (e) {
      log.warning('[Analysis] FFmpeg analyzer cache age check failed: $e');
      return false;
    }
  }

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

  Future<AnalysisError> _generationFailureError({
    required String hash,
    required String fileName,
    required int maxCacheBytes,
    bool forceIncomplete = false,
  }) async {
    if (maxCacheBytes > 0) {
      final snapshot = await AnalysisCache.snapshot(maxBytes: maxCacheBytes);
      final incomplete =
          forceIncomplete || AnalysisCache.hasIncompleteContainer(hash);
      if (incomplete && snapshot.isOverLimit) {
        log.warning(
          '[Analysis] generation left incomplete VAC while cache is full: '
          'current=${snapshot.totalBytes}, max=$maxCacheBytes, hash=$hash',
        );
        return AnalysisError(AnalysisErrorKey.cacheWriteIncomplete, [
          fileName,
          AnalysisCache.formatBytes(snapshot.totalBytes),
          AnalysisCache.formatBytes(maxCacheBytes),
        ]);
      }
    }
    return AnalysisError(AnalysisErrorKey.unsupported, [fileName]);
  }

  void _setTrackStatus(
    String path, {
    required String fileName,
    String? hash,
    required AnalysisTrackStatus status,
    required double progress,
    AnalysisError? error,
  }) {
    _trackStatusByPath[path] = AnalysisTrackGenerationStatus(
      path: path,
      fileName: fileName,
      hash: hash ?? _trackStatusByPath[path]?.hash,
      status: status,
      progress: progress,
      error: error,
    );
    notifyListeners();
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
