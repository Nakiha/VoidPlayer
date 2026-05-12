import 'dart:async';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;

import '../app_log.dart';
import '../utils/file_lock.dart';
import 'analysis_cache_service.dart';
import 'analysis_ffi.dart';
import 'analysis_generation_queue.dart';
import 'analysis_generation_settings.dart';
import 'analysis_native_service.dart';
import 'analysis_overlay.dart';
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

class AnalysisOverlayTrackSource {
  final String hash;
  final String name;
  final String path;
  final int trackFileId;

  const AnalysisOverlayTrackSource({
    required this.hash,
    required this.name,
    required this.path,
    required this.trackFileId,
  });
}

abstract class AnalysisGenerationService {
  String? get activeOverlayHash;
  bool get overlayPanelVisible;
  Set<int> get activeOverlayTrackFileIds;
  AnalysisOverlayConfig get overlayConfig;
  AnalysisTrackGenerationStatus? statusForPath(String path);
  Future<String?> ensureGenerated(String videoPath);
  Future<bool> ensureOverlayChunk(String hash, {required String videoPath});
  Future<bool> activateOverlay(
    String hash, {
    required String name,
    required String path,
    required int trackFileId,
  });
  Future<bool> activateOverlayTracks(List<AnalysisOverlayTrackSource> tracks);
  void updateOverlayConfig(AnalysisOverlayConfig config);
  void deactivateOverlay();
}

/// Dart-side state machine for the analysis generation + loading flow.
///
/// The UI (AnalysisPanel) listens to this via [ChangeNotifier] to show
/// progress / error / loaded states.
class AnalysisManager extends ChangeNotifier
    implements AnalysisGenerationService {
  AnalysisManager._({
    AnalysisGenerationSettings settings =
        const AppConfigAnalysisGenerationSettings(),
    AnalysisCacheService cache = const DefaultAnalysisCacheService(),
    AnalysisNativeService native = const DefaultAnalysisNativeService(),
    AnalysisGenerationQueue? generationQueue,
  }) : _settings = settings,
       _cache = cache,
       _native = native,
       _generationQueue =
           generationQueue ??
           SerialAnalysisGenerationQueue(cache: cache, native: native);

  static final AnalysisManager instance = AnalysisManager._();

  final AnalysisGenerationSettings _settings;
  final AnalysisCacheService _cache;
  final AnalysisNativeService _native;
  final AnalysisGenerationQueue _generationQueue;
  AnalysisState _state = AnalysisState.idle;
  AnalysisError? _error;
  String? _generatingFileName;
  String? _loadedHash;
  String? _activeOverlayHash;
  int _activeOverlayTrackFileId = -1;
  final Map<int, String> _activeOverlayHashesByTrackFileId = {};
  final Map<int, FileLockHandle> _overlayHashLocksByTrackFileId = {};
  AnalysisOverlayConfig _overlayConfig = const AnalysisOverlayConfig();
  FileLockHandle? _loadedHashLock;
  final Map<String, Future<String?>> _ensureGeneratedInFlightByPath = {};
  final Map<String, Future<bool>> _ensureOverlayChunkInFlightByKey = {};
  final Map<String, AnalysisTrackGenerationStatus> _trackStatusByPath = {};
  int _stateSerial = 0;
  int _loadSerial = 0;
  int _ensureAndLoadSerial = 0;

  AnalysisState get state => _state;
  AnalysisError? get error => _error;
  String? get generatingFileName => _generatingFileName;
  String? get loadedHash => _loadedHash;
  @override
  String? get activeOverlayHash => _activeOverlayHash;
  @override
  bool get overlayPanelVisible => _activeOverlayHashesByTrackFileId.isNotEmpty;
  @override
  Set<int> get activeOverlayTrackFileIds =>
      Set<int>.unmodifiable(_activeOverlayHashesByTrackFileId.keys);
  @override
  AnalysisOverlayConfig get overlayConfig => _overlayConfig;
  bool get isLoaded => _state == AnalysisState.loaded;

  @override
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
  @override
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

  @override
  Future<bool> ensureOverlayChunk(String hash, {required String videoPath}) {
    final targetFrame = _resolveOverlayTargetFrame(hash);
    final range = _overlayChunkRangeFor(hash, targetFrame);
    if (range == null) return Future.value(false);
    if (_cache.hasOverlayChunkForFrame(hash, targetFrame)) {
      return Future.value(true);
    }

    final key = '$hash:${range.startFrame}:${range.endFrame}';
    final existing = _ensureOverlayChunkInFlightByKey[key];
    if (existing != null) return existing;

    late final Future<bool> future;
    future =
        _ensureOverlayChunkImpl(
          hash: hash,
          videoPath: videoPath,
          startFrame: range.startFrame,
          endFrame: range.endFrame,
          targetFrame: targetFrame,
        ).whenComplete(() {
          if (identical(_ensureOverlayChunkInFlightByKey[key], future)) {
            _ensureOverlayChunkInFlightByKey.remove(key);
          }
        });
    _ensureOverlayChunkInFlightByKey[key] = future;
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

    final indexedHash = await _cache.findHashForUnchangedVideo(videoPath);
    if (indexedHash != null && _hasUsableCacheEntry(indexedHash, videoPath)) {
      log.info('[Analysis] metadata cache hit for $indexedHash');
      await _refreshCacheEntry(indexedHash, fileName, videoPath);
      await _cache.touchEntry(indexedHash);
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
    _setStateIfCurrent(stateSerial, AnalysisState.computingHash);
    _setTrackStatus(
      videoPath,
      fileName: fileName,
      status: AnalysisTrackStatus.computingHash,
      progress: 0,
    );
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
      await _cache.touchEntry(hash);
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
    _unloadHashBeforeRegeneration(hash);
    log.info('[Analysis] cache miss, will generate');

    final maxCacheBytes = _settings.maxCacheBytes;
    if (maxCacheBytes > 0) {
      final pruneResult = await _cache.enforceLimit(
        maxBytes: maxCacheBytes,
        protectedHashes: {hash},
      );
      if (pruneResult.snapshot.isOverLimit) {
        log.warning(
          '[Analysis] cache limit reached: '
          'current=${pruneResult.snapshot.totalBytes}, max=$maxCacheBytes',
        );
        final error = AnalysisError(AnalysisErrorKey.cacheLimitExceeded, [
          _cache.formatBytes(pruneResult.snapshot.totalBytes),
          _cache.formatBytes(maxCacheBytes),
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
      '[Analysis] calling FFI generate VAC2 base(videoPath=$videoPath, hash=$hash)',
    );
    final bool ok;
    try {
      ok = await _generateAnalysisSerialized(videoPath, hash);
    } catch (e, stack) {
      log.severe('[Analysis] generate VAC2 base threw: $e', e, stack);
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
      log.severe('[Analysis] generate VAC2 base returned false');
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
    log.info('[Analysis] generate VAC2 base succeeded');

    if (!_cache.filesExist(hash)) {
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
      final pruneResult = await _cache.enforceLimit(
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
    if (_loadedHash == hash) {
      _activeOverlayHash = null;
      _activeOverlayTrackFileId = -1;
      _applyDisabledOverlayConfig();
      _native.unload();
      _loadedHash = null;
      _releaseLoadedHashLock();
    }
    if (_cache.deleteIfVacVersionMismatch(hash)) {
      log.info('[Analysis] deleted stale VAC version before load: $hash');
      final regeneratedHash = await ensureGenerated(path);
      if (regeneratedHash == null) return false;
      hash = regeneratedHash;
    }
    final serial = ++_loadSerial;
    _setState(AnalysisState.loading);
    _setTrackStatus(
      path,
      fileName: name,
      hash: hash,
      status: AnalysisTrackStatus.loading,
      progress: 1,
    );
    final analysisPath = _cache.analysisPath(hash);

    log.info('[Analysis] loading: analysis=$analysisPath');
    final hashLock = _cache.acquireHashSharedLockSync(hash);
    final bool ok;
    try {
      ok = await _native
          .load(analysisPath)
          .timeout(const Duration(seconds: 45));
    } on TimeoutException catch (e, stack) {
      hashLock.releaseSync();
      log.severe('[Analysis] FFI load timed out: $analysisPath', e, stack);
      final error = AnalysisError(AnalysisErrorKey.loadFailed, [name]);
      _setTrackStatus(
        path,
        fileName: name,
        hash: hash,
        status: AnalysisTrackStatus.error,
        progress: 1,
        error: error,
      );
      if (_isLoadCurrent(serial)) {
        _setErrorObject(error);
      }
      return false;
    } catch (_) {
      hashLock.releaseSync();
      rethrow;
    }
    if (!_isLoadCurrent(serial)) {
      hashLock.releaseSync();
      return false;
    }
    if (!ok) {
      hashLock.releaseSync();
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

    _releaseLoadedHashLock();
    _loadedHash = hash;
    _loadedHashLock = hashLock;
    await _cache.touchEntry(hash);
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

  @override
  Future<bool> activateOverlay(
    String hash, {
    required String name,
    required String path,
    required int trackFileId,
  }) async {
    return activateOverlayTracks([
      AnalysisOverlayTrackSource(
        hash: hash,
        name: name,
        path: path,
        trackFileId: trackFileId,
      ),
    ]);
  }

  @override
  Future<bool> activateOverlayTracks(
    List<AnalysisOverlayTrackSource> tracks,
  ) async {
    _releaseOverlayHashLocks();
    AnalysisFfi.clearOverlayTracks();
    _activeOverlayHashesByTrackFileId.clear();
    _activeOverlayHash = null;
    _activeOverlayTrackFileId = -1;

    var activatedAny = false;
    for (final track in tracks) {
      if (_cache.deleteIfVacVersionMismatch(track.hash)) {
        log.info('[Analysis] skipped stale overlay VAC version: ${track.hash}');
        continue;
      }
      final chunkReady = await ensureOverlayChunk(
        track.hash,
        videoPath: track.path,
      );
      if (!chunkReady) {
        log.info(
          '[Analysis] skipped overlay for ${track.hash}: '
          'VAC2 cache has no overlay chunk for the current frame',
        );
        continue;
      }
      final analysisPath = _cache.analysisPath(track.hash);
      final lock = _cache.acquireHashSharedLockSync(track.hash);
      final loaded = AnalysisFfi.setOverlayTrack(
        trackFileId: track.trackFileId,
        analysisPath: analysisPath,
      );
      if (!loaded) {
        lock.releaseSync();
        log.warning(
          '[Analysis] failed to load overlay track '
          '${track.trackFileId}: $analysisPath',
        );
        continue;
      }
      _overlayHashLocksByTrackFileId[track.trackFileId] = lock;
      _activeOverlayHashesByTrackFileId[track.trackFileId] = track.hash;
      _activeOverlayHash ??= track.hash;
      if (_activeOverlayTrackFileId < 0) {
        _activeOverlayTrackFileId = track.trackFileId;
      }
      _setTrackStatus(
        track.path,
        fileName: track.name,
        hash: track.hash,
        status: AnalysisTrackStatus.cached,
        progress: 1,
      );
      await _cache.touchEntry(track.hash);
      activatedAny = true;
    }

    if (activatedAny) {
      _applyOverlayConfig();
    } else {
      _applyDisabledOverlayConfig();
    }
    notifyListeners();
    return activatedAny;
  }

  @override
  void updateOverlayConfig(AnalysisOverlayConfig config) {
    _overlayConfig = config;
    if (overlayPanelVisible) {
      _applyOverlayConfig();
    }
    notifyListeners();
  }

  @override
  void deactivateOverlay() {
    if (!overlayPanelVisible && _activeOverlayHash == null) return;
    _activeOverlayHash = null;
    _activeOverlayTrackFileId = -1;
    _activeOverlayHashesByTrackFileId.clear();
    _releaseOverlayHashLocks();
    AnalysisFfi.clearOverlayTracks();
    _applyDisabledOverlayConfig();
    notifyListeners();
  }

  void unload() {
    _ensureAndLoadSerial++;
    _stateSerial++;
    _loadSerial++;
    _ensureGeneratedInFlightByPath.clear();
    _ensureOverlayChunkInFlightByKey.clear();
    _activeOverlayHash = null;
    _activeOverlayTrackFileId = -1;
    _activeOverlayHashesByTrackFileId.clear();
    _releaseOverlayHashLocks();
    AnalysisFfi.clearOverlayTracks();
    _applyDisabledOverlayConfig();
    if (_state == AnalysisState.loaded) {
      _native.unload();
    }
    _loadedHash = null;
    _releaseLoadedHashLock();
    _setState(AnalysisState.idle);
  }

  void _releaseLoadedHashLock() {
    _loadedHashLock?.releaseSync();
    _loadedHashLock = null;
  }

  void _releaseOverlayHashLocks() {
    for (final lock in _overlayHashLocksByTrackFileId.values) {
      lock.releaseSync();
    }
    _overlayHashLocksByTrackFileId.clear();
  }

  void _applyOverlayConfig() {
    final config = _overlayConfig;
    AnalysisFfi.setOverlay(
      showCuGrid: config.showCuGrid,
      showPredMode: config.showPredMode,
      showQpHeatmap: config.showQpHeatmap,
      showPredLines: config.showPredLines,
      showCuBitCostHeatmap: config.showCuBitCostHeatmap,
      opacity: config.opacity,
      mode: config.type.index,
      trackFileId: -1,
    );
  }

  void _applyDisabledOverlayConfig() {
    AnalysisFfi.setOverlay(
      showCuGrid: false,
      showPredMode: false,
      showQpHeatmap: false,
      showPredLines: false,
      showCuBitCostHeatmap: false,
      opacity: _overlayConfig.opacity,
      mode: _overlayConfig.type.index,
      trackFileId: -1,
    );
  }

  void _unloadHashBeforeRegeneration(String hash) {
    final overlayUsesHash = _activeOverlayHashesByTrackFileId.containsValue(
      hash,
    );
    if (_loadedHash != hash && !overlayUsesHash) return;
    log.info('[Analysis] unloading stale cache before regeneration: $hash');
    if (overlayUsesHash) {
      _activeOverlayHash = null;
      _activeOverlayTrackFileId = -1;
      _activeOverlayHashesByTrackFileId.clear();
      _releaseOverlayHashLocks();
      AnalysisFfi.clearOverlayTracks();
      _applyDisabledOverlayConfig();
    }
    if (_loadedHash == hash) {
      _native.unload();
      _loadedHash = null;
      _releaseLoadedHashLock();
    }
    notifyListeners();
  }

  // ---- Internal ----

  void _setState(AnalysisState s) {
    if (_state == s && _error == null && _generatingFileName == null) {
      return;
    }
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
    if (_cache.deleteIfVacVersionMismatch(hash)) {
      log.info('[Analysis] deleted stale VAC version: $hash');
      return false;
    }
    if (!_cache.hasEntry(hash, videoPath: videoPath)) return false;
    final analysisPath = _cache.analysisPath(hash);

    AnalysisSession? session;
    try {
      session = _native.openSession(analysisPath);
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
      final analysisPath = _cache.analysisPath(hash);
      if (p.basename(analysisPath).toLowerCase() == 'base.vac') {
        return false;
      }
      final analysisFile = File(analysisPath);
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
    return _generationQueue.generate(
      videoPath: videoPath,
      hash: hash,
      maxCacheBytes: _settings.maxCacheBytes,
    );
  }

  Future<bool> _ensureOverlayChunkImpl({
    required String hash,
    required String videoPath,
    required int startFrame,
    required int endFrame,
    required int targetFrame,
  }) async {
    if (_cache.deleteIfVacVersionMismatch(hash) || !_cache.filesExist(hash)) {
      return false;
    }
    if (_cache.hasOverlayChunkForFrame(hash, targetFrame)) return true;

    final maxCacheBytes = _settings.maxCacheBytes;
    if (maxCacheBytes > 0) {
      final pruneResult = await _cache.enforceLimit(
        maxBytes: maxCacheBytes,
        protectedHashes: {hash},
      );
      if (pruneResult.snapshot.isOverLimit) {
        log.warning(
          '[Analysis] cannot generate overlay chunk; cache limit reached: '
          'current=${pruneResult.snapshot.totalBytes}, max=$maxCacheBytes',
        );
        return false;
      }
    }

    log.info(
      '[Analysis] calling FFI generate VAC2 overlay chunk('
      'videoPath=$videoPath, hash=$hash, frames=$startFrame..$endFrame)',
    );
    final ok = await _generationQueue.generateOverlayChunk(
      videoPath: videoPath,
      hash: hash,
      startFrame: startFrame,
      endFrame: endFrame,
      maxCacheBytes: maxCacheBytes,
    );
    if (!ok) {
      log.warning(
        '[Analysis] generate VAC2 overlay chunk returned false: '
        'hash=$hash frames=$startFrame..$endFrame',
      );
      return false;
    }
    await _cache.touchEntry(hash);
    return _cache.hasOverlayChunkForFrame(hash, targetFrame);
  }

  int _resolveOverlayTargetFrame(String hash) {
    AnalysisSession? session;
    try {
      session = _native.openSession(_cache.analysisPath(hash));
      final summary = session?.summary;
      if (summary == null || summary.loaded == 0 || summary.frameCount <= 0) {
        return 0;
      }
      final current = summary.currentFrameIdx;
      if (current < 0) return 0;
      return current.clamp(0, summary.frameCount - 1).toInt();
    } catch (e, stack) {
      log.warning(
        '[Analysis] failed to resolve overlay frame for $hash: $e',
        e,
        stack,
      );
      return 0;
    } finally {
      session?.close();
    }
  }

  ({int startFrame, int endFrame})? _overlayChunkRangeFor(
    String hash,
    int targetFrame,
  ) {
    AnalysisSession? session;
    try {
      session = _native.openSession(_cache.analysisPath(hash));
      final summary = session?.summary;
      if (summary == null || summary.loaded == 0 || summary.frameCount <= 0) {
        return null;
      }
      final frameCount = summary.frameCount;
      final safeTarget = targetFrame.clamp(0, frameCount - 1).toInt();
      const chunkFrameCount = 64;
      final start = (safeTarget - chunkFrameCount ~/ 2)
          .clamp(0, frameCount - 1)
          .toInt();
      final end = (start + chunkFrameCount - 1)
          .clamp(start, frameCount - 1)
          .toInt();
      return (startFrame: start, endFrame: end);
    } catch (e, stack) {
      log.warning(
        '[Analysis] failed to resolve overlay chunk range for $hash: $e',
        e,
        stack,
      );
      return null;
    } finally {
      session?.close();
    }
  }

  Future<AnalysisError> _generationFailureError({
    required String hash,
    required String fileName,
    required int maxCacheBytes,
    bool forceIncomplete = false,
  }) async {
    if (maxCacheBytes > 0) {
      final snapshot = await _cache.snapshot(maxBytes: maxCacheBytes);
      final incomplete = forceIncomplete || _cache.hasIncompleteContainer(hash);
      if (incomplete && snapshot.isOverLimit) {
        log.warning(
          '[Analysis] generation left incomplete VAC while cache is full: '
          'current=${snapshot.totalBytes}, max=$maxCacheBytes, hash=$hash',
        );
        return AnalysisError(AnalysisErrorKey.cacheWriteIncomplete, [
          fileName,
          _cache.formatBytes(snapshot.totalBytes),
          _cache.formatBytes(maxCacheBytes),
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
    final previous = _trackStatusByPath[path];
    final nextHash = hash ?? previous?.hash;
    if (previous != null &&
        previous.fileName == fileName &&
        previous.hash == nextHash &&
        previous.status == status &&
        previous.progress == progress &&
        previous.error == error) {
      return;
    }
    _trackStatusByPath[path] = AnalysisTrackGenerationStatus(
      path: path,
      fileName: fileName,
      hash: nextHash,
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
      await _cache.addEntry(hash, fileName, videoPath);
    } catch (e, stack) {
      log.warning('[Analysis] failed to refresh cache index: $e', e, stack);
    }
  }

  static Future<String> _computeHash(String path) => computeFileSha256(path);
}
