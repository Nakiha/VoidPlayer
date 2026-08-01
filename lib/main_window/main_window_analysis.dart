import 'dart:async';

import 'package:flutter/foundation.dart';

import '../analysis/analysis_manager.dart';
import '../analysis/analysis_overlay.dart';
import '../analysis/ui/workspace/analysis_workspace_models.dart';
import '../app_log.dart';
import '../native_player/native_player_protocol.dart';
import '../track_manager.dart';
import '../utils/async_guard.dart';
import 'main_window_state.dart';

class MainWindowAnalysisCoordinator {
  static const Duration _overlayPlaybackPrefetchInterval = Duration(
    milliseconds: 750,
  );

  final TrackManager trackManager;
  final MainWindowStateStore stateStore;
  final AnalysisGenerationService analysisGeneration;
  final bool analysisOverlaysEnabled;
  final Future<PresentedFrameTiming?> Function(int fileId)?
  presentedFrameProvider;
  final void Function()? onOverlayStateChanged;
  final Map<int, String> _hashesByFileId = <int, String>{};
  final ValueNotifier<List<AnalysisWorkspaceEntry>> _entries = ValueNotifier(
    const <AnalysisWorkspaceEntry>[],
  );

  bool _disposed = false;
  bool _overlayPanelRequested = false;
  Future<void>? _operationInFlight;
  Timer? _overlayPlaybackPrefetchTimer;
  late final Listenable? _analysisGenerationListenable;
  late int _observedOverlayPresentationRevision;
  int _latestSeekOverlayRequestId = 0;

  MainWindowAnalysisCoordinator({
    required this.trackManager,
    required this.stateStore,
    this.analysisOverlaysEnabled = true,
    AnalysisGenerationService? analysisGeneration,
    this.presentedFrameProvider,
    this.onOverlayStateChanged,
  }) : analysisGeneration = analysisGeneration ?? AnalysisManager.instance {
    _observedOverlayPresentationRevision =
        this.analysisGeneration.overlayPresentationRevision;
    _analysisGenerationListenable = this.analysisGeneration is Listenable
        ? this.analysisGeneration as Listenable
        : null;
    _analysisGenerationListenable?.addListener(
      _handleAnalysisGenerationChanged,
    );
    trackManager.addListener(_handleTrackManagerChanged);
    _syncEntries();
  }

  ValueListenable<List<AnalysisWorkspaceEntry>> get entries => _entries;

  Future<void> dispose() async {
    _disposed = true;
    _analysisGenerationListenable?.removeListener(
      _handleAnalysisGenerationChanged,
    );
    trackManager.removeListener(_handleTrackManagerChanged);
    _stopOverlayPlaybackPrefetch();
    _hashesByFileId.clear();
    _entries.dispose();
  }

  Future<void> enterAnalysis() {
    return _enqueueOperation(_enterAnalysisImpl);
  }

  Future<void> toggleOverlay(TrackEntry track, String hash) {
    if (!analysisOverlaysEnabled) return Future.value();
    return _enqueueOperation(() => _toggleOverlayImpl(track, hash));
  }

  Future<void> toggleOverlayPanel() {
    if (!analysisOverlaysEnabled) return Future.value();
    return _enqueueOperation(_toggleOverlayPanelImpl);
  }

  Future<void> activateOverlayPanelTracks() {
    if (!analysisOverlaysEnabled) return Future.value();
    return _enqueueOperation(() async {
      _overlayPanelRequested = true;
      await _syncOverlayPanelTracksImpl();
    });
  }

  Future<String?> ensureGeneratedForSlot(int slotIndex) {
    return _enqueueValueOperation(() async {
      if (slotIndex < 0 || slotIndex >= trackManager.entries.length) {
        return null;
      }
      final entry = trackManager.entries[slotIndex];
      final hash = await analysisGeneration.ensureGeneratedAndLoaded(
        entry.path,
      );
      if (_disposed || hash == null) return null;
      _hashesByFileId[entry.fileId] = hash;
      _syncEntries();
      return hash;
    });
  }

  Future<void> syncOverlayPanelTracks() {
    if (!analysisOverlaysEnabled) return Future.value();
    return _enqueueOperation(_syncOverlayPanelTracksImpl);
  }

  Future<void> refreshOverlayForCurrentFrame() {
    if (!analysisOverlaysEnabled) return Future.value();
    return _enqueueOperation(_refreshOverlayForCurrentFrameImpl);
  }

  void beginSeekOverlayRefresh(int requestId) {
    if (_disposed) return;
    _latestSeekOverlayRequestId = requestId;
  }

  Future<void> refreshOverlayForPresentedFrame({
    required int requestId,
    required int trackFileId,
    required int ptsUs,
    required int dtsUs,
  }) {
    if (!analysisOverlaysEnabled) return Future.value();
    if (requestId != _latestSeekOverlayRequestId) return Future.value();
    return _enqueueOperation(
      () => _refreshOverlayForCurrentFrameImpl(
        presentedFrameOverrides: {
          trackFileId: PresentedFrameTiming(ptsUs: ptsUs, dtsUs: dtsUs),
        },
        isCurrent: () => requestId == _latestSeekOverlayRequestId,
      ),
    );
  }

  Future<void> toggleOverlayForSlot(int slotIndex) {
    return _enqueueOperation(() async {
      if (!analysisOverlaysEnabled) return;
      if (slotIndex < 0 || slotIndex >= trackManager.entries.length) return;
      final track = trackManager.entries[slotIndex];
      if (analysisGeneration.activeOverlayTrackFileIds.contains(track.fileId)) {
        _overlayPanelRequested = false;
        _stopOverlayPlaybackPrefetch();
        analysisGeneration.deactivateOverlay();
        _notifyOverlayStateChanged();
        return;
      }
      final hash = await analysisGeneration.ensureGenerated(track.path);
      if (_disposed || hash == null) return;
      await _toggleOverlayImpl(track, hash);
    });
  }

  void updateOverlayConfig(AnalysisOverlayConfig config) {
    if (_disposed) return;
    if (!analysisOverlaysEnabled) return;
    analysisGeneration.updateOverlayConfig(config);
    _notifyOverlayStateChanged();
  }

  void deactivateOverlay() {
    if (_disposed) return;
    _overlayPanelRequested = false;
    _stopOverlayPlaybackPrefetch();
    analysisGeneration.deactivateOverlay();
    _notifyOverlayStateChanged();
  }

  Future<void> _toggleOverlayPanelImpl() async {
    if (!analysisOverlaysEnabled) return;
    if (trackManager.isEmpty) return;
    if (_overlayPanelRequested || analysisGeneration.overlayPanelVisible) {
      _overlayPanelRequested = false;
      _stopOverlayPlaybackPrefetch();
      analysisGeneration.deactivateOverlay();
      _notifyOverlayStateChanged();
      return;
    }
    _overlayPanelRequested = true;
    await _syncOverlayPanelTracksImpl();
  }

  Future<void> _syncOverlayPanelTracksImpl() async {
    if (!_overlayPanelRequested) return;
    final requestedFileIds = trackManager.entries
        .map((entry) => entry.fileId)
        .toSet();
    await _activateOverlayTrackFileIdsImpl(requestedFileIds);
  }

  Future<void> _activateOverlayTrackFileIdsImpl(
    Set<int> requestedFileIds,
  ) async {
    final requestedEntries = trackManager.entries
        .where((entry) => requestedFileIds.contains(entry.fileId))
        .toList(growable: false);
    if (requestedEntries.isEmpty) {
      _overlayPanelRequested = false;
      _stopOverlayPlaybackPrefetch();
      analysisGeneration.deactivateOverlay();
      _notifyOverlayStateChanged();
      return;
    }
    _overlayPanelRequested = true;

    final sources = <AnalysisOverlayTrackSource>[];
    for (final entry in requestedEntries) {
      final hash = await analysisGeneration.ensureGenerated(entry.path);
      if (_disposed || !_overlayPanelRequested) return;
      if (hash == null) continue;
      _hashesByFileId[entry.fileId] = hash;
      _syncEntries();
      final presentedFrame = await _presentedFrameForTrack(entry);
      if (_disposed || !_overlayPanelRequested) return;
      sources.add(
        AnalysisOverlayTrackSource(
          hash: hash,
          name: entry.fileName,
          path: entry.path,
          trackFileId: entry.fileId,
          analysisFrameIndex: presentedFrame?.analysisFrameIndex,
          frameIdentityMode: presentedFrame?.frameIdentityMode,
          sourcePacketIndex: presentedFrame?.sourcePacketIndex,
          sourcePacketSize: presentedFrame?.sourcePacketSize,
          sourcePacketPos: presentedFrame?.sourcePacketPos,
          sourcePacketPtsUs: presentedFrame?.sourcePacketPtsUs,
          sourcePacketDtsUs: presentedFrame?.sourcePacketDtsUs,
          presentedPtsUs: presentedFrame?.ptsUs,
          presentedDtsUs: presentedFrame?.dtsUs,
        ),
      );
    }

    if (_disposed || !_overlayPanelRequested) return;
    final liveEntriesByFileId = {
      for (final entry in trackManager.entries) entry.fileId: entry,
    };
    final liveSources = sources
        .where((source) {
          final entry = liveEntriesByFileId[source.trackFileId];
          return entry != null && entry.path == source.path;
        })
        .toList(growable: false);
    if (liveSources.isEmpty) {
      _overlayPanelRequested = false;
      _stopOverlayPlaybackPrefetch();
      analysisGeneration.deactivateOverlay();
      _notifyOverlayStateChanged();
      return;
    }
    final activated = await analysisGeneration.activateOverlayTracks(
      liveSources,
    );
    if (_disposed || !_overlayPanelRequested) return;
    if (activated) {
      _startOverlayPlaybackPrefetch();
      _notifyOverlayStateChanged();
    } else {
      _overlayPanelRequested = false;
      _stopOverlayPlaybackPrefetch();
      analysisGeneration.deactivateOverlay();
      _notifyOverlayStateChanged();
    }
  }

  Future<void> _enterAnalysisImpl() async {
    if (trackManager.isEmpty) return;
    _syncEntries();
    stateStore.setDeckCollapsed(false);
    stateStore.setDeckTab(MainWindowDeckTab.analysis);
    final requestedEntries = trackManager.entries.toList(growable: false);
    await _ensureAnalysisEntries(requestedEntries);
  }

  Future<void> _ensureAnalysisEntries(List<TrackEntry> requestedEntries) async {
    // AnalysisManager's cache metadata connection is process-scoped. Keep
    // multi-track generation deterministic instead of issuing concurrent
    // metadata/cache operations now that analysis lives in the main process.
    for (final entry in requestedEntries) {
      final hash = await analysisGeneration.ensureGenerated(entry.path);
      if (_disposed) return;
      if (hash == null) continue;
      final stillOpen = trackManager.entries.any(
        (current) =>
            current.fileId == entry.fileId && current.path == entry.path,
      );
      if (!stillOpen) continue;
      _hashesByFileId[entry.fileId] = hash;
      _syncEntries();
    }
  }

  void _handleTrackManagerChanged() {
    _syncEntries();
    if (stateStore.value.deckTab != MainWindowDeckTab.analysis ||
        trackManager.isEmpty) {
      return;
    }
    final unresolved = trackManager.entries
        .where((entry) {
          if (_hashesByFileId.containsKey(entry.fileId)) return false;
          return _readyHash(analysisGeneration.statusForPath(entry.path)) ==
              null;
        })
        .toList(growable: false);
    if (unresolved.isEmpty) return;
    fireAndLogFine(
      'generate analysis for added tracks',
      _enqueueOperation(() => _ensureAnalysisEntries(unresolved)),
    );
  }

  Future<void> _toggleOverlayImpl(TrackEntry track, String hash) async {
    final stillOpen = trackManager.entries.any(
      (entry) => entry.fileId == track.fileId && entry.path == track.path,
    );
    if (!stillOpen) return;
    if (analysisGeneration.activeOverlayTrackFileIds.contains(track.fileId)) {
      _overlayPanelRequested = false;
      _stopOverlayPlaybackPrefetch();
      analysisGeneration.deactivateOverlay();
      _notifyOverlayStateChanged();
      return;
    }
    final generatedHash = await analysisGeneration.ensureGenerated(track.path);
    if (_disposed || generatedHash == null) return;
    _overlayPanelRequested = true;
    await _activateOverlayImpl(track, generatedHash);
  }

  Future<void> _activateOverlayImpl(TrackEntry track, String hash) async {
    final presentedFrame = await _presentedFrameForTrack(track);
    if (_disposed) return;
    final activated = await analysisGeneration.activateOverlay(
      hash,
      name: track.fileName,
      path: track.path,
      trackFileId: track.fileId,
      analysisFrameIndex: presentedFrame?.analysisFrameIndex,
      sourcePacketIndex: presentedFrame?.sourcePacketIndex,
      sourcePacketSize: presentedFrame?.sourcePacketSize,
      sourcePacketPos: presentedFrame?.sourcePacketPos,
      sourcePacketPtsUs: presentedFrame?.sourcePacketPtsUs,
      sourcePacketDtsUs: presentedFrame?.sourcePacketDtsUs,
      presentedPtsUs: presentedFrame?.ptsUs,
      presentedDtsUs: presentedFrame?.dtsUs,
    );
    if (_disposed) return;
    if (!activated) {
      _overlayPanelRequested = false;
      _stopOverlayPlaybackPrefetch();
      analysisGeneration.deactivateOverlay();
      _notifyOverlayStateChanged();
      return;
    }
    _hashesByFileId[track.fileId] = hash;
    _syncEntries();
    _startOverlayPlaybackPrefetch();
    _notifyOverlayStateChanged();
  }

  Future<void> _refreshOverlayForCurrentFrameImpl({
    Map<int, PresentedFrameTiming>? presentedFrameOverrides,
    bool notifyOnSuccess = true,
    bool Function()? isCurrent,
  }) async {
    if (isCurrent != null && !isCurrent()) return;
    final activeFileIds = analysisGeneration.activeOverlayTrackFileIds;
    if (activeFileIds.isEmpty) return;

    final sources = <AnalysisOverlayTrackSource>[];
    for (final entry in trackManager.entries) {
      if (!activeFileIds.contains(entry.fileId)) continue;
      var hash = _hashesByFileId[entry.fileId];
      hash ??= await analysisGeneration.ensureGenerated(entry.path);
      if (_disposed || (isCurrent != null && !isCurrent())) return;
      if (hash == null) continue;
      _hashesByFileId[entry.fileId] = hash;
      _syncEntries();
      final presentedFrame =
          presentedFrameOverrides?[entry.fileId] ??
          await _presentedFrameForTrack(entry);
      if (_disposed || (isCurrent != null && !isCurrent())) return;
      sources.add(
        AnalysisOverlayTrackSource(
          hash: hash,
          name: entry.fileName,
          path: entry.path,
          trackFileId: entry.fileId,
          analysisFrameIndex: presentedFrame?.analysisFrameIndex,
          frameIdentityMode: presentedFrame?.frameIdentityMode,
          sourcePacketIndex: presentedFrame?.sourcePacketIndex,
          sourcePacketSize: presentedFrame?.sourcePacketSize,
          sourcePacketPos: presentedFrame?.sourcePacketPos,
          sourcePacketPtsUs: presentedFrame?.sourcePacketPtsUs,
          sourcePacketDtsUs: presentedFrame?.sourcePacketDtsUs,
          presentedPtsUs: presentedFrame?.ptsUs,
          presentedDtsUs: presentedFrame?.dtsUs,
        ),
      );
    }

    if (_disposed || sources.isEmpty || (isCurrent != null && !isCurrent())) {
      return;
    }
    final refreshed = await analysisGeneration.activateOverlayTracks(sources);
    if (_disposed || !refreshed || (isCurrent != null && !isCurrent())) {
      return;
    }
    if (notifyOnSuccess) {
      _notifyOverlayStateChanged();
    }
  }

  void _startOverlayPlaybackPrefetch() {
    if (_overlayPlaybackPrefetchTimer != null) return;
    _overlayPlaybackPrefetchTimer = Timer.periodic(
      _overlayPlaybackPrefetchInterval,
      (_) => _tickOverlayPlaybackPrefetch(),
    );
  }

  void _stopOverlayPlaybackPrefetch() {
    _overlayPlaybackPrefetchTimer?.cancel();
    _overlayPlaybackPrefetchTimer = null;
  }

  void _tickOverlayPlaybackPrefetch() {
    if (_disposed ||
        !_overlayPanelRequested ||
        !analysisGeneration.overlayPanelVisible ||
        analysisGeneration.activeOverlayTrackFileIds.isEmpty) {
      _stopOverlayPlaybackPrefetch();
      return;
    }
    if (_operationInFlight != null) return;
    fireAndLogFine(
      'overlay playback prefetch',
      _enqueueOperation(
        () => _refreshOverlayForCurrentFrameImpl(notifyOnSuccess: false),
      ),
    );
  }

  Future<PresentedFrameTiming?> _presentedFrameForTrack(
    TrackEntry track,
  ) async {
    final provider = presentedFrameProvider;
    if (provider == null) return null;
    try {
      return await provider(track.fileId);
    } catch (error, stack) {
      log.fine(
        'presented frame lookup failed: fileId=${track.fileId}',
        error,
        stack,
      );
      return null;
    }
  }

  void _syncEntries() {
    if (_disposed) return;
    final liveFileIds = trackManager.entries.map((e) => e.fileId).toSet();
    _hashesByFileId.removeWhere((fileId, _) => !liveFileIds.contains(fileId));
    final next = <AnalysisWorkspaceEntry>[
      for (final entry in trackManager.entries)
        AnalysisWorkspaceEntry(
          fileId: entry.fileId,
          path: entry.path,
          fileName: entry.fileName,
          hash:
              _hashesByFileId[entry.fileId] ??
              _readyHash(analysisGeneration.statusForPath(entry.path)),
          generationStatus: analysisGeneration.statusForPath(entry.path),
        ),
    ];
    if (listEquals(_entries.value, next)) return;
    _entries.value = List.unmodifiable(next);
  }

  String? _readyHash(AnalysisTrackGenerationStatus? status) =>
      (status?.isCached ?? false) ? status?.hash : null;

  Future<void> _enqueueOperation(Future<void> Function() operation) {
    if (_disposed) return Future.value();
    final previous = _operationInFlight;
    late final Future<void> future;
    future = (previous ?? Future<void>.value())
        .catchError((Object error, StackTrace stack) {
          log.warning('previous analysis operation failed', error, stack);
        })
        .then((_) async {
          if (_disposed) return;
          await operation();
        })
        .whenComplete(() {
          if (identical(_operationInFlight, future)) {
            _operationInFlight = null;
          }
        });
    _operationInFlight = future;
    return future;
  }

  Future<T?> _enqueueValueOperation<T>(Future<T?> Function() operation) {
    if (_disposed) return Future.value();
    final previous = _operationInFlight;
    late final Future<T?> future;
    late final Future<void> completion;
    future = (previous ?? Future<void>.value())
        .catchError((Object error, StackTrace stack) {
          log.warning('previous analysis value operation failed', error, stack);
        })
        .then((_) async {
          if (_disposed) return null;
          return operation();
        });
    completion = future.whenComplete(() {
      if (identical(_operationInFlight, completion)) {
        _operationInFlight = null;
      }
    });
    _operationInFlight = completion;
    return future;
  }

  void _notifyOverlayStateChanged() {
    if (_disposed) return;
    onOverlayStateChanged?.call();
  }

  void _handleAnalysisGenerationChanged() {
    if (_disposed) return;
    _syncEntries();
    final revision = analysisGeneration.overlayPresentationRevision;
    if (revision == _observedOverlayPresentationRevision) return;
    _observedOverlayPresentationRevision = revision;
    if (!analysisOverlaysEnabled) return;
    if (!analysisGeneration.overlayPanelVisible ||
        analysisGeneration.activeOverlayTrackFileIds.isEmpty) {
      return;
    }
    _notifyOverlayStateChanged();
  }
}
