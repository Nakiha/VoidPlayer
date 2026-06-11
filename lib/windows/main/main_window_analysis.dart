import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;

import '../../analysis/analysis_manager.dart';
import '../../analysis/analysis_overlay.dart';
import '../../app_log.dart';
import '../../native_player/native_player_protocol.dart';
import '../../platform/analysis_process_host.dart';
import '../../track_manager.dart';
import '../../utils/async_guard.dart';
import '../analysis/ipc/analysis_ipc_models.dart';
import '../analysis/ipc/analysis_ipc_server.dart';

class MainWindowAnalysisCoordinator {
  static const Duration _overlayPlaybackPrefetchInterval = Duration(
    milliseconds: 750,
  );

  final TrackManager trackManager;
  final AnalysisProcessHost analysisProcesses;
  final AnalysisGenerationService analysisGeneration;
  final bool analysisOverlaysEnabled;
  final Future<PresentedFrameTiming?> Function(int fileId)?
  presentedFrameProvider;
  final void Function()? onOverlayStateChanged;
  final AnalysisIpcServer _ipcServer = AnalysisIpcServer();
  final Map<int, String> _hashesByFileId = <int, String>{};

  bool _disposed = false;
  bool _overlayPanelRequested = false;
  Future<void>? _operationInFlight;
  Timer? _overlayPlaybackPrefetchTimer;
  late final Listenable? _analysisGenerationListenable;
  late int _observedOverlayPresentationRevision;

  MainWindowAnalysisCoordinator({
    required this.trackManager,
    required this.analysisProcesses,
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
    _ipcServer.publishAccentColor(analysisProcesses.accentColorValue);
  }

  Future<void> dispose() async {
    _disposed = true;
    _analysisGenerationListenable?.removeListener(
      _handleAnalysisGenerationChanged,
    );
    _stopOverlayPlaybackPrefetch();
    _hashesByFileId.clear();
    analysisProcesses.analysisIpcPort = null;
    analysisProcesses.analysisIpcToken = null;
    await _ipcServer.dispose();
  }

  Future<void> triggerAnalysis() {
    return _enqueueOperation(_triggerAnalysisImpl);
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

  Future<void> refreshOverlayForPresentedFrame({
    required int trackFileId,
    required int ptsUs,
    required int dtsUs,
  }) {
    if (!analysisOverlaysEnabled) return Future.value();
    return _enqueueOperation(
      () => _refreshOverlayForCurrentFrameImpl(
        presentedFrameOverrides: {
          trackFileId: PresentedFrameTiming(ptsUs: ptsUs, dtsUs: dtsUs),
        },
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

  Future<void> _triggerAnalysisImpl() async {
    if (trackManager.isEmpty) return;
    if (!analysisProcesses.supportsExternalAnalysisWindows) return;
    if (analysisProcesses.activateAnalysisWindows()) return;
    final windows = <AnalysisWindowRequest>[];
    await _ipcServer.start();
    if (_disposed) return;
    _ipcServer.publishAccentColor(analysisProcesses.accentColorValue);
    analysisProcesses.analysisIpcPort = _ipcServer.port;
    analysisProcesses.analysisIpcToken = _ipcServer.token;
    for (final entry in trackManager.entries) {
      final hash = await analysisGeneration.ensureGenerated(entry.path);
      if (_disposed) return;
      if (hash != null) {
        _hashesByFileId[entry.fileId] = hash;
        windows.add((hash: hash, fileName: p.basename(entry.path)));
      }
    }
    await _publishTrackSnapshotImpl();
    if (_disposed) return;
    await analysisProcesses.showAnalysisWindows(
      windows,
      onExit: _handleAnalysisWindowExited,
    );
  }

  Future<void> publishTrackSnapshot() async {
    return _enqueueOperation(_publishTrackSnapshotImpl);
  }

  void publishAccentColor(int colorValue) {
    if (_disposed) return;
    analysisProcesses.accentColorValue = colorValue;
    _ipcServer.publishAccentColor(colorValue);
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
    _startOverlayPlaybackPrefetch();
    _notifyOverlayStateChanged();
  }

  Future<void> _refreshOverlayForCurrentFrameImpl({
    Map<int, PresentedFrameTiming>? presentedFrameOverrides,
    bool notifyOnSuccess = true,
  }) async {
    final activeFileIds = analysisGeneration.activeOverlayTrackFileIds;
    if (activeFileIds.isEmpty) return;

    final sources = <AnalysisOverlayTrackSource>[];
    for (final entry in trackManager.entries) {
      if (!activeFileIds.contains(entry.fileId)) continue;
      var hash = _hashesByFileId[entry.fileId];
      hash ??= await analysisGeneration.ensureGenerated(entry.path);
      if (_disposed) return;
      if (hash == null) continue;
      _hashesByFileId[entry.fileId] = hash;
      final presentedFrame =
          presentedFrameOverrides?[entry.fileId] ??
          await _presentedFrameForTrack(entry);
      if (_disposed) return;
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

    if (_disposed || sources.isEmpty) return;
    final refreshed = await analysisGeneration.activateOverlayTracks(sources);
    if (_disposed || !refreshed) return;
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

  Future<void> _publishTrackSnapshotImpl() async {
    if (!_ipcServer.isStarted) return;
    if (!_ipcServer.hasClients) return;
    final tracks = <AnalysisIpcTrack>[];
    final liveFileIds = trackManager.entries.map((e) => e.fileId).toSet();
    _hashesByFileId.removeWhere((fileId, _) => !liveFileIds.contains(fileId));

    for (final entry in trackManager.entries) {
      var hash = _hashesByFileId[entry.fileId];
      if (hash == null) {
        hash = await analysisGeneration.ensureGenerated(entry.path);
        if (_disposed) return;
        if (hash == null) continue;
        _hashesByFileId[entry.fileId] = hash;
      }
      if (_disposed) return;
      tracks.add(
        AnalysisIpcTrack(
          fileId: entry.fileId,
          slot: entry.slot,
          path: entry.path,
          fileName: entry.fileName,
          hash: hash,
          durationUs: entry.info.durationUs,
        ),
      );
    }

    if (_disposed) return;
    _ipcServer.publishTracks(tracks);
  }

  void _handleAnalysisWindowExited() {
    fireAndLogFine(
      'deactivate analysis IPC workspace',
      _enqueueOperation(_deactivateIpcWorkspace),
    );
  }

  Future<void> _deactivateIpcWorkspace() async {
    final port = _ipcServer.port;
    final token = _ipcServer.token;
    await _ipcServer.dispose();
    _hashesByFileId.clear();
    if (_disposed) return;
    if (analysisProcesses.analysisIpcPort == port &&
        analysisProcesses.analysisIpcToken == token) {
      analysisProcesses.analysisIpcPort = null;
      analysisProcesses.analysisIpcToken = null;
    }
  }

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
