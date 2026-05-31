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
import 'analysis_overlay_chunk_scheduler.dart';
import 'file_hash.dart';
import 'nalu_types.dart';

const int _noTimestampUs = -9223372036854775808;

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
  final int? analysisFrameIndex;
  final int? frameIdentityMode;
  final int? sourcePacketIndex;
  final int? sourcePacketSize;
  final int? sourcePacketPos;
  final int? sourcePacketPtsUs;
  final int? sourcePacketDtsUs;
  final int? presentedPtsUs;
  final int? presentedDtsUs;

  const AnalysisOverlayTrackSource({
    required this.hash,
    required this.name,
    required this.path,
    required this.trackFileId,
    this.analysisFrameIndex,
    this.frameIdentityMode,
    this.sourcePacketIndex,
    this.sourcePacketSize,
    this.sourcePacketPos,
    this.sourcePacketPtsUs,
    this.sourcePacketDtsUs,
    this.presentedPtsUs,
    this.presentedDtsUs,
  });
}

class _AnalysisCancelToken {
  bool cancelled = false;
}

class _OverlayTrackRequest {
  final AnalysisOverlayTrackSource source;
  final int targetFrame;
  final List<({int startFrame, int endFrame})> ranges;

  const _OverlayTrackRequest({
    required this.source,
    required this.targetFrame,
    required this.ranges,
  });
}

class _ReadyOverlayTrackState {
  final String hash;
  final List<({int startFrame, int endFrame})> indexedRanges;

  const _ReadyOverlayTrackState({
    required this.hash,
    required this.indexedRanges,
  });

  bool covers(int frame) {
    for (final range in indexedRanges) {
      if (frame >= range.startFrame && frame <= range.endFrame) return true;
    }
    return false;
  }
}

abstract class AnalysisGenerationService {
  String? get activeOverlayHash;
  bool get overlayPanelVisible;
  Set<int> get activeOverlayTrackFileIds;
  AnalysisOverlayConfig get overlayConfig;
  int get overlayPresentationRevision;
  AnalysisTrackGenerationStatus? statusForPath(String path);
  bool supportsOverlayForHash(String hash);
  Future<String?> ensureGenerated(String videoPath);
  Future<String?> ensureGeneratedAndLoaded(String videoPath);
  Future<bool> ensureOverlayChunk(
    String hash, {
    required String videoPath,
    int? analysisFrameIndex,
    int? sourcePacketIndex,
    int? sourcePacketSize,
    int? sourcePacketPos,
    int? sourcePacketPtsUs,
    int? sourcePacketDtsUs,
    int? presentedPtsUs,
    int? presentedDtsUs,
  });
  Future<bool> activateOverlay(
    String hash, {
    required String name,
    required String path,
    required int trackFileId,
    int? analysisFrameIndex,
    int? sourcePacketIndex,
    int? sourcePacketSize,
    int? sourcePacketPos,
    int? sourcePacketPtsUs,
    int? sourcePacketDtsUs,
    int? presentedPtsUs,
    int? presentedDtsUs,
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
  final Map<int, _OverlayTrackRequest> _requestedOverlayTracksByFileId = {};
  final Map<int, _ReadyOverlayTrackState> _readyOverlayTracksByFileId = {};
  final Map<int, FileLockHandle> _overlayHashLocksByTrackFileId = {};
  AnalysisOverlayConfig _overlayConfig = const AnalysisOverlayConfig();
  FileLockHandle? _loadedHashLock;
  final Map<String, Future<String?>> _ensureGeneratedInFlightByPath = {};
  late final AnalysisOverlayChunkScheduler _overlayChunkScheduler =
      AnalysisOverlayChunkScheduler(
        maxWorkers:
            AnalysisOverlayChunkScheduler.defaultNativeSubmissionWorkers(),
        onComplete: _handleOverlayChunkJobComplete,
        onLog: (message, [error, stackTrace]) {
          if (error == null) {
            log.info(message);
          } else {
            log.warning(message, error, stackTrace);
          }
        },
      );
  final Map<String, AnalysisTrackGenerationStatus> _trackStatusByPath = {};
  _AnalysisCancelToken _cancelToken = _AnalysisCancelToken();
  int _stateSerial = 0;
  int _loadSerial = 0;
  int _ensureAndLoadSerial = 0;
  int _overlayActivationSerial = 0;
  int _overlayPresentationRevision = 0;

  AnalysisState get state => _state;
  AnalysisError? get error => _error;
  String? get generatingFileName => _generatingFileName;
  String? get loadedHash => _loadedHash;
  int get analysisWorkerCount => _overlayChunkScheduler.maxWorkers;
  int get analysisActiveWorkers => _overlayChunkScheduler.activeWorkers;
  int get analysisPendingJobs => _overlayChunkScheduler.pendingJobs;
  int get analysisBackpressureDropCount =>
      _overlayChunkScheduler.backpressureDropCount;
  @override
  String? get activeOverlayHash => _activeOverlayHash;
  @override
  bool get overlayPanelVisible => _requestedOverlayTracksByFileId.isNotEmpty;
  @override
  Set<int> get activeOverlayTrackFileIds =>
      Set<int>.unmodifiable(_requestedOverlayTracksByFileId.keys);
  @override
  AnalysisOverlayConfig get overlayConfig => _overlayConfig;
  @override
  int get overlayPresentationRevision => _overlayPresentationRevision;
  bool get isLoaded => _state == AnalysisState.loaded;

  @override
  AnalysisTrackGenerationStatus? statusForPath(String path) =>
      _trackStatusByPath[path];

  @override
  bool supportsOverlayForHash(String hash) => _supportsOverlayForHash(hash);

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

    final cancelToken = _cancelToken;
    late final Future<String?> future;
    future = _ensureGeneratedImpl(videoPath, cancelToken).whenComplete(() {
      if (identical(_ensureGeneratedInFlightByPath[videoPath], future)) {
        _ensureGeneratedInFlightByPath.remove(videoPath);
      }
    });
    _ensureGeneratedInFlightByPath[videoPath] = future;
    return future;
  }

  @override
  Future<bool> ensureOverlayChunk(
    String hash, {
    required String videoPath,
    int? analysisFrameIndex,
    int? sourcePacketIndex,
    int? sourcePacketSize,
    int? sourcePacketPos,
    int? sourcePacketPtsUs,
    int? sourcePacketDtsUs,
    int? presentedPtsUs,
    int? presentedDtsUs,
  }) async {
    final targetFrame = _resolveOverlayTargetFrame(
      hash,
      analysisFrameIndex: analysisFrameIndex,
      sourcePacketIndex: sourcePacketIndex,
      sourcePacketSize: sourcePacketSize,
      sourcePacketPos: sourcePacketPos,
      sourcePacketPtsUs: sourcePacketPtsUs,
      sourcePacketDtsUs: sourcePacketDtsUs,
      presentedPtsUs: presentedPtsUs,
      presentedDtsUs: presentedDtsUs,
    );
    if (!_supportsOverlayForHash(hash)) {
      return false;
    }
    final ranges = _overlayChunkRangesFor(hash, targetFrame);
    if (ranges == null) return false;
    final futures = <Future<bool>>[];
    for (final range in ranges) {
      final frame = targetFrame.clamp(range.startFrame, range.endFrame).toInt();
      if (_cache.hasOverlayChunkForFrame(hash, frame)) {
        continue;
      }
      futures.add(
        _ensureOverlayChunkRange(
          hash: hash,
          videoPath: videoPath,
          startFrame: range.startFrame,
          endFrame: range.endFrame,
          targetFrame: frame,
          priority: 0,
        ),
      );
    }
    if (futures.isEmpty) return true;
    final results = await Future.wait(futures);
    return results.every((ok) => ok) &&
        _cache.hasOverlayChunkForFrame(hash, targetFrame);
  }

  Future<bool> _ensureOverlayChunkRange({
    required String hash,
    required String videoPath,
    required int startFrame,
    required int endFrame,
    required int targetFrame,
    required int priority,
    int? overlaySerial,
  }) {
    if (_cache.hasOverlayChunkForFrame(hash, targetFrame)) {
      return Future.value(true);
    }
    final request = AnalysisOverlayChunkRequest(
      hash: hash,
      videoPath: videoPath,
      startFrame: startFrame,
      endFrame: endFrame,
      targetFrame: targetFrame,
    );
    final cancelToken = _cancelToken;
    return _overlayChunkScheduler.schedule(
      request: request,
      priority: priority,
      overlaySerial: overlaySerial,
      run: () => _ensureOverlayChunkImpl(
        hash: hash,
        videoPath: videoPath,
        startFrame: startFrame,
        endFrame: endFrame,
        targetFrame: targetFrame,
        cancelToken: cancelToken,
      ),
    );
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

  @override
  Future<String?> ensureGeneratedAndLoaded(String videoPath) =>
      ensureAndLoad(videoPath);

  Future<String?> _ensureGeneratedImpl(
    String videoPath,
    _AnalysisCancelToken cancelToken,
  ) async {
    final fileName = p.basename(videoPath);
    final stateSerial = ++_stateSerial;
    log.info('[Analysis] ensureGenerated: videoPath=$videoPath');

    final indexedHash = await _cache.findHashForUnchangedVideo(videoPath);
    if (cancelToken.cancelled) return null;
    if (indexedHash != null && _hasUsableCacheEntry(indexedHash, videoPath)) {
      log.info('[Analysis] metadata cache hit for $indexedHash');
      await _refreshCacheEntry(indexedHash, fileName, videoPath);
      if (cancelToken.cancelled) return null;
      await _cache.touchEntry(indexedHash);
      if (cancelToken.cancelled) return null;
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: indexedHash,
        status: AnalysisTrackStatus.cached,
        progress: 1,
        cancelToken: cancelToken,
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
      cancelToken: cancelToken,
    );
    try {
      hash = await _computeHash(videoPath);
      if (cancelToken.cancelled) return null;
      log.info('[Analysis] hash=$hash');
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: hash,
        status: AnalysisTrackStatus.computingHash,
        progress: 0,
        cancelToken: cancelToken,
      );
    } catch (e) {
      if (cancelToken.cancelled) return null;
      log.severe('[Analysis] hash failed: $e');
      final error = AnalysisError(AnalysisErrorKey.hashFailed, ['$e']);
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        status: AnalysisTrackStatus.error,
        progress: 0,
        error: error,
        cancelToken: cancelToken,
      );
      if (_isStateCurrent(stateSerial)) {
        _setErrorObject(error);
      }
      return null;
    }

    if (_hasUsableCacheEntry(hash, videoPath)) {
      log.info('[Analysis] cache hit for $hash');
      await _refreshCacheEntry(hash, fileName, videoPath);
      if (cancelToken.cancelled) return null;
      await _cache.touchEntry(hash);
      if (cancelToken.cancelled) return null;
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: hash,
        status: AnalysisTrackStatus.cached,
        progress: 1,
        cancelToken: cancelToken,
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
        protectedHashes: _protectedHashes(hash),
      );
      if (cancelToken.cancelled) return null;
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
          cancelToken: cancelToken,
        );
        if (_isStateCurrent(stateSerial)) {
          _setErrorObject(error);
        }
        return null;
      }
    }

    if (!cancelToken.cancelled && _isStateCurrent(stateSerial)) {
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
      cancelToken: cancelToken,
    );

    log.info(
      '[Analysis] calling FFI generate VAC2 base(videoPath=$videoPath, hash=$hash)',
    );
    final bool ok;
    try {
      ok = await _generateVac2BaseSerialized(videoPath, hash);
      if (cancelToken.cancelled) return null;
    } catch (e, stack) {
      log.severe('[Analysis] generate VAC2 base threw: $e', e, stack);
      final error = await _generationFailureError(
        hash: hash,
        fileName: fileName,
        maxCacheBytes: maxCacheBytes,
      );
      if (cancelToken.cancelled) return null;
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: hash,
        status: AnalysisTrackStatus.error,
        progress: 0,
        error: error,
        cancelToken: cancelToken,
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
      if (cancelToken.cancelled) return null;
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: hash,
        status: AnalysisTrackStatus.error,
        progress: 0,
        error: error,
        cancelToken: cancelToken,
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
      if (cancelToken.cancelled) return null;
      _setTrackStatus(
        videoPath,
        fileName: fileName,
        hash: hash,
        status: AnalysisTrackStatus.error,
        progress: 0,
        error: error,
        cancelToken: cancelToken,
      );
      if (_isStateCurrent(stateSerial)) {
        _setErrorObject(error);
      }
      return null;
    }

    await _refreshCacheEntry(hash, fileName, videoPath);
    if (cancelToken.cancelled) return null;
    log.info('[Analysis] index entry saved');

    if (maxCacheBytes > 0) {
      final pruneResult = await _cache.enforceLimit(
        maxBytes: maxCacheBytes,
        protectedHashes: _protectedHashes(hash),
      );
      if (cancelToken.cancelled) return null;
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
      cancelToken: cancelToken,
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
      _overlayActivationSerial++;
      _clearOverlayState();
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
    int? analysisFrameIndex,
    int? sourcePacketIndex,
    int? sourcePacketSize,
    int? sourcePacketPos,
    int? sourcePacketPtsUs,
    int? sourcePacketDtsUs,
    int? presentedPtsUs,
    int? presentedDtsUs,
  }) async {
    return activateOverlayTracks([
      AnalysisOverlayTrackSource(
        hash: hash,
        name: name,
        path: path,
        trackFileId: trackFileId,
        analysisFrameIndex: analysisFrameIndex,
        sourcePacketIndex: sourcePacketIndex,
        sourcePacketSize: sourcePacketSize,
        sourcePacketPos: sourcePacketPos,
        sourcePacketPtsUs: sourcePacketPtsUs,
        sourcePacketDtsUs: sourcePacketDtsUs,
        presentedPtsUs: presentedPtsUs,
        presentedDtsUs: presentedDtsUs,
      ),
    ]);
  }

  @override
  Future<bool> activateOverlayTracks(
    List<AnalysisOverlayTrackSource> tracks,
  ) async {
    final serial = ++_overlayActivationSerial;

    final requests = <int, _OverlayTrackRequest>{};
    for (final track in tracks) {
      if (_cache.deleteIfVacVersionMismatch(track.hash)) {
        log.info('[Analysis] skipped stale overlay VAC version: ${track.hash}');
        continue;
      }
      if (!_supportsOverlayForHash(track.hash)) {
        log.info(
          '[Analysis] skipped overlay for ${track.hash}: '
          'codec does not support VACHUNK overlay generation',
        );
        continue;
      }
      final targetFrame = _resolveOverlayTargetFrame(
        track.hash,
        analysisFrameIndex: track.analysisFrameIndex,
        sourcePacketIndex: track.sourcePacketIndex,
        sourcePacketSize: track.sourcePacketSize,
        sourcePacketPos: track.sourcePacketPos,
        sourcePacketPtsUs: track.sourcePacketPtsUs,
        sourcePacketDtsUs: track.sourcePacketDtsUs,
        presentedPtsUs: track.presentedPtsUs,
        presentedDtsUs: track.presentedDtsUs,
      );
      final ranges = _overlayChunkRangesFor(
        track.hash,
        targetFrame,
        forwardPrefetchWindows: 1,
      );
      if (ranges == null) {
        log.info(
          '[Analysis] skipped overlay for ${track.hash}: '
          'VAC2 cache cannot resolve overlay chunk range',
        );
        continue;
      }
      requests[track.trackFileId] = _OverlayTrackRequest(
        source: track,
        targetFrame: targetFrame,
        ranges: ranges,
      );
    }

    final trackSetChanged = _overlayTrackSetChanged(requests);
    if (trackSetChanged) {
      _clearOverlayState();
    }
    _requestedOverlayTracksByFileId.addAll(requests);
    _activeOverlayHash = requests.values.isEmpty
        ? null
        : requests.values.first.source.hash;

    if (requests.isEmpty) {
      _applyDisabledOverlayConfig();
      notifyListeners();
      return false;
    }

    _applyOverlayConfig();
    final needsReload =
        trackSetChanged ||
        requests.values.any(_overlayRequestNeedsNativeReload);
    if (needsReload) {
      _reloadReadyOverlayTracksForIntent(serial, reason: 'activate');
    }
    for (final request in requests.values) {
      _scheduleOverlayChunksForRequest(request, serial);
    }
    notifyListeners();
    return true;
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
    _overlayActivationSerial++;
    _clearOverlayChunkScheduler();
    _clearOverlayState();
    _applyDisabledOverlayConfig();
    notifyListeners();
  }

  void unload() {
    _ensureAndLoadSerial++;
    _stateSerial++;
    _loadSerial++;
    _overlayActivationSerial++;
    _cancelPendingWork();
    _ensureGeneratedInFlightByPath.clear();
    _clearOverlayChunkScheduler();
    _clearOverlayState();
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

  void _clearOverlayState() {
    _activeOverlayHash = null;
    _requestedOverlayTracksByFileId.clear();
    _readyOverlayTracksByFileId.clear();
    AnalysisFfi.clearOverlayTracks();
    _releaseOverlayHashLocks();
  }

  void _cancelPendingWork() {
    _cancelToken.cancelled = true;
    _cancelToken = _AnalysisCancelToken();
  }

  bool _isOverlayActivationCurrent(int serial) =>
      serial == _overlayActivationSerial;

  void _clearOverlayChunkScheduler() {
    _overlayChunkScheduler.clear();
  }

  bool _overlayTrackSetChanged(Map<int, _OverlayTrackRequest> requests) {
    if (_requestedOverlayTracksByFileId.length != requests.length) {
      return true;
    }
    for (final entry in requests.entries) {
      final previous = _requestedOverlayTracksByFileId[entry.key]?.source;
      final next = entry.value.source;
      if (previous == null ||
          previous.hash != next.hash ||
          previous.path != next.path) {
        return true;
      }
    }
    return false;
  }

  bool _overlayRequestNeedsNativeReload(_OverlayTrackRequest request) {
    final ready = _readyOverlayTracksByFileId[request.source.trackFileId];
    if (ready == null || ready.hash != request.source.hash) return true;
    if (!ready.covers(request.targetFrame)) return true;
    return false;
  }

  List<({int startFrame, int endFrame})> _readyRangesForRequest(
    _OverlayTrackRequest request,
  ) {
    final ranges = <({int startFrame, int endFrame})>[];
    for (final range in request.ranges) {
      final probeFrame = request.targetFrame
          .clamp(range.startFrame, range.endFrame)
          .toInt();
      if (_cache.hasOverlayChunkForFrame(request.source.hash, probeFrame)) {
        ranges.add(range);
      }
    }
    return ranges;
  }

  void _handleOverlayChunkJobComplete(AnalysisOverlayChunkJobResult result) {
    final serial = result.overlaySerial;
    if (!result.ok || serial == null || !_isOverlayActivationCurrent(serial)) {
      return;
    }
    final loadedAny = _reloadReadyOverlayTracksForIntent(
      serial,
      reason: 'chunk-ready',
    );
    if (loadedAny) {
      _overlayPresentationRevision++;
    }
    notifyListeners();
  }

  void _scheduleOverlayChunksForRequest(
    _OverlayTrackRequest request,
    int serial,
  ) {
    for (final range in request.ranges) {
      final targetFrame = request.targetFrame
          .clamp(range.startFrame, range.endFrame)
          .toInt();
      if (_cache.hasOverlayChunkForFrame(request.source.hash, targetFrame)) {
        continue;
      }
      final containsPresentedFrame =
          request.targetFrame >= range.startFrame &&
          request.targetFrame <= range.endFrame;
      unawaited(
        _ensureOverlayChunkRange(
          hash: request.source.hash,
          videoPath: request.source.path,
          startFrame: range.startFrame,
          endFrame: range.endFrame,
          targetFrame: targetFrame,
          priority: containsPresentedFrame ? 0 : 10,
          overlaySerial: serial,
        ).then((ok) {
          if (!ok && _isOverlayActivationCurrent(serial)) {
            log.info(
              '[Analysis] overlay chunk not ready yet for '
              '${request.source.hash} frames=${range.startFrame}..${range.endFrame}',
            );
          }
        }),
      );
    }
  }

  bool _reloadReadyOverlayTracksForIntent(
    int serial, {
    required String reason,
  }) {
    if (!_isOverlayActivationCurrent(serial)) return false;

    AnalysisFfi.clearOverlayTracks();
    _readyOverlayTracksByFileId.clear();
    _releaseOverlayHashLocks();

    var loadedAny = false;
    for (final request in _requestedOverlayTracksByFileId.values) {
      final track = request.source;
      final readyRanges = _readyRangesForRequest(request);
      if (readyRanges.isEmpty ||
          !readyRanges.any((range) {
            return request.targetFrame >= range.startFrame &&
                request.targetFrame <= range.endFrame;
          })) {
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
      _readyOverlayTracksByFileId[track.trackFileId] = _ReadyOverlayTrackState(
        hash: track.hash,
        indexedRanges: readyRanges,
      );
      _setTrackStatus(
        track.path,
        fileName: track.name,
        hash: track.hash,
        status: AnalysisTrackStatus.cached,
        progress: 1,
      );
      unawaited(
        _cache.touchEntry(track.hash).catchError((Object e, StackTrace stack) {
          log.warning(
            '[Analysis] failed to touch overlay cache ${track.hash}: $e',
            e,
            stack,
          );
        }),
      );
      loadedAny = true;
    }

    if (loadedAny) {
      log.info(
        '[Analysis] overlay tracks loaded after $reason: '
        '${_readyOverlayTracksByFileId.keys.toList()}',
      );
    }
    if (_requestedOverlayTracksByFileId.isNotEmpty) {
      _applyOverlayConfig();
    }
    return loadedAny;
  }

  Set<String> _protectedHashes(String hash) {
    final hashes = <String>{
      hash,
      ..._requestedOverlayTracksByFileId.values.map(
        (request) => request.source.hash,
      ),
      ..._readyOverlayTracksByFileId.values.map((ready) => ready.hash),
    };
    final loadedHash = _loadedHash;
    if (loadedHash != null) hashes.add(loadedHash);
    return hashes;
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
    final overlayUsesHash =
        _requestedOverlayTracksByFileId.values.any(
          (request) => request.source.hash == hash,
        ) ||
        _readyOverlayTracksByFileId.values.any((ready) => ready.hash == hash);
    if (_loadedHash != hash && !overlayUsesHash) return;
    log.info('[Analysis] unloading stale cache before regeneration: $hash');
    if (overlayUsesHash) {
      _overlayActivationSerial++;
      _clearOverlayChunkScheduler();
      _clearOverlayState();
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
      if (summary.videoWidth <= 0 || summary.videoHeight <= 0) {
        log.info(
          '[Analysis] cache stale for $hash: '
          'invalid video dimensions ${summary.videoWidth}x${summary.videoHeight}',
        );
        return false;
      }

      final codec = analysisCodecFromValue(summary.codec);
      if (_requiresFrameData(codec) && summary.frameCount <= 0) {
        log.info(
          '[Analysis] cache stale for $hash: codec=${analysisCodecName(codec)} '
          'requires VAC2 frame index data',
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
    if (codec != AnalysisCodec.h264 &&
        codec != AnalysisCodec.hevc &&
        codec != AnalysisCodec.vvc) {
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

  Future<bool> _generateVac2BaseSerialized(String videoPath, String hash) {
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
    required _AnalysisCancelToken cancelToken,
  }) async {
    if (cancelToken.cancelled) return false;
    if (_cache.deleteIfVacVersionMismatch(hash) || !_cache.filesExist(hash)) {
      return false;
    }
    if (_cache.hasOverlayChunkForFrame(hash, targetFrame)) return true;

    final maxCacheBytes = _settings.maxCacheBytes;
    if (maxCacheBytes > 0) {
      final pruneResult = await _cache.enforceLimit(
        maxBytes: maxCacheBytes,
        protectedHashes: _protectedHashes(hash),
      );
      if (cancelToken.cancelled) return false;
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
    if (cancelToken.cancelled) return false;
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

  int _resolveOverlayTargetFrame(
    String hash, {
    int? analysisFrameIndex,
    int? sourcePacketIndex,
    int? sourcePacketSize,
    int? sourcePacketPos,
    int? sourcePacketPtsUs,
    int? sourcePacketDtsUs,
    int? presentedPtsUs,
    int? presentedDtsUs,
  }) {
    AnalysisSession? session;
    try {
      session = _native.openSession(_cache.analysisPath(hash));
      final summary = session?.summary;
      if (summary == null || summary.loaded == 0 || summary.frameCount <= 0) {
        return 0;
      }
      var current = -1;
      if (analysisFrameIndex != null &&
          analysisFrameIndex >= 0 &&
          analysisFrameIndex < summary.frameCount) {
        current = analysisFrameIndex;
      }
      if (current < 0 && sourcePacketPos != null && sourcePacketPos >= 0) {
        current =
            session?.frameIndexForSourcePacket(
              packetPos: sourcePacketPos,
              packetSize: sourcePacketSize ?? 0,
              packetIndex: sourcePacketIndex ?? -1,
              packetPts: sourcePacketPtsUs ?? _noTimestampUs,
              packetDts: sourcePacketDtsUs ?? _noTimestampUs,
            ) ??
            -1;
      }
      if (current < 0 &&
          presentedPtsUs != null &&
          presentedPtsUs >= 0 &&
          presentedDtsUs != null &&
          presentedDtsUs != _noTimestampUs) {
        current =
            session?.frameIndexForTimestamp(
              ptsUs: presentedPtsUs,
              dtsUs: presentedDtsUs,
            ) ??
            -1;
        if (current >= 0) {
          log.fine(
            '[Analysis] resolved overlay frame by timestamp fallback: '
            'hash=$hash frame=$current pts=$presentedPtsUs dts=$presentedDtsUs',
          );
        }
      }
      if (current < 0) {
        current = summary.currentFrameIdx;
      }
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

  List<({int startFrame, int endFrame})>? _overlayChunkRangesFor(
    String hash,
    int targetFrame, {
    int forwardPrefetchWindows = 0,
  }) {
    AnalysisSession? session;
    try {
      session = _native.openSession(_cache.analysisPath(hash));
      final summary = session?.summary;
      if (summary == null || summary.loaded == 0 || summary.frameCount <= 0) {
        return null;
      }
      final frameCount = summary.frameCount;
      return overlayChunkRangesForFrame(
        frameCount: frameCount,
        targetFrame: targetFrame,
        forwardPrefetchWindows: forwardPrefetchWindows,
      );
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

  bool _supportsOverlayForHash(String hash) {
    AnalysisSession? session;
    try {
      session = _native.openSession(_cache.analysisPath(hash));
      final summary = session?.summary;
      if (summary == null || summary.loaded == 0) return false;
      final codec = analysisCodecFromValue(summary.codec);
      final supported = supportsOverlayCodec(codec);
      if (!supported) {
        log.info(
          '[Analysis] overlay VACHUNK unsupported for '
          '${analysisCodecName(codec)} cache $hash',
        );
      }
      return supported;
    } catch (e, stack) {
      log.warning(
        '[Analysis] failed to check overlay support for $hash: $e',
        e,
        stack,
      );
      return false;
    } finally {
      session?.close();
    }
  }

  @visibleForTesting
  static bool supportsOverlayCodec(AnalysisCodec codec) => switch (codec) {
    AnalysisCodec.h264 || AnalysisCodec.hevc || AnalysisCodec.vvc => true,
    AnalysisCodec.unknown ||
    AnalysisCodec.av1 ||
    AnalysisCodec.vp9 ||
    AnalysisCodec.mpeg2 => false,
  };

  @visibleForTesting
  static ({int startFrame, int endFrame}) overlayChunkRangeForFrame({
    required int frameCount,
    required int targetFrame,
    int chunkFrameCount = 64,
  }) {
    final safeFrameCount = frameCount <= 0 ? 1 : frameCount;
    final safeChunkFrameCount = chunkFrameCount <= 0 ? 64 : chunkFrameCount;
    final safeTarget = targetFrame.clamp(0, safeFrameCount - 1).toInt();
    final start = (safeTarget ~/ safeChunkFrameCount) * safeChunkFrameCount;
    final end = (start + safeChunkFrameCount - 1)
        .clamp(start, safeFrameCount - 1)
        .toInt();
    return (startFrame: start, endFrame: end);
  }

  @visibleForTesting
  static List<({int startFrame, int endFrame})> overlayChunkRangesForFrame({
    required int frameCount,
    required int targetFrame,
    int chunkFrameCount = 64,
    int edgePrefetchDivisor = 4,
    int forwardPrefetchWindows = 0,
  }) {
    final safeFrameCount = frameCount <= 0 ? 1 : frameCount;
    final safeChunkFrameCount = chunkFrameCount <= 0 ? 64 : chunkFrameCount;
    final safeTarget = targetFrame.clamp(0, safeFrameCount - 1).toInt();
    final edgePrefetchSize = (safeChunkFrameCount ~/ edgePrefetchDivisor)
        .clamp(1, safeChunkFrameCount)
        .toInt();
    final current = overlayChunkRangeForFrame(
      frameCount: safeFrameCount,
      targetFrame: safeTarget,
      chunkFrameCount: safeChunkFrameCount,
    );
    final ranges = <({int startFrame, int endFrame})>[current];
    final localFrame = safeTarget - current.startFrame;
    final currentWindowSize = current.endFrame - current.startFrame + 1;

    if (localFrame < edgePrefetchSize && current.startFrame > 0) {
      ranges.add(
        overlayChunkRangeForFrame(
          frameCount: safeFrameCount,
          targetFrame: current.startFrame - 1,
          chunkFrameCount: safeChunkFrameCount,
        ),
      );
    }
    if (localFrame >= currentWindowSize - edgePrefetchSize &&
        current.endFrame < safeFrameCount - 1) {
      ranges.add(
        overlayChunkRangeForFrame(
          frameCount: safeFrameCount,
          targetFrame: current.endFrame + 1,
          chunkFrameCount: safeChunkFrameCount,
        ),
      );
    }
    for (var i = 1; i <= forwardPrefetchWindows; i++) {
      final nextFrame = current.endFrame + (i - 1) * safeChunkFrameCount + 1;
      if (nextFrame >= safeFrameCount) break;
      ranges.add(
        overlayChunkRangeForFrame(
          frameCount: safeFrameCount,
          targetFrame: nextFrame,
          chunkFrameCount: safeChunkFrameCount,
        ),
      );
    }

    final seen = <String>{};
    return ranges
        .where((range) => seen.add('${range.startFrame}:${range.endFrame}'))
        .toList(growable: false);
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
    _AnalysisCancelToken? cancelToken,
  }) {
    if (cancelToken?.cancelled ?? false) return;
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
