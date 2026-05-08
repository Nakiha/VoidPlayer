import 'dart:async';

import 'package:path/path.dart' as p;

import '../../analysis/analysis_manager.dart';
import '../../analysis/analysis_overlay.dart';
import '../../track_manager.dart';
import '../analysis/ipc/analysis_ipc_models.dart';
import '../analysis/ipc/analysis_ipc_server.dart';
import '../window_manager.dart';

class MainWindowAnalysisCoordinator {
  final TrackManager trackManager;
  final AnalysisProcessManager analysisProcesses;
  final AnalysisGenerationService analysisGeneration;
  final AnalysisIpcServer _ipcServer = AnalysisIpcServer();
  final Map<int, String> _hashesByFileId = <int, String>{};

  int _opSerial = 0;
  bool _disposed = false;
  Future<void>? _operationInFlight;

  MainWindowAnalysisCoordinator({
    required this.trackManager,
    required this.analysisProcesses,
    AnalysisGenerationService? analysisGeneration,
  }) : analysisGeneration = analysisGeneration ?? AnalysisManager.instance {
    _ipcServer.publishAccentColor(analysisProcesses.accentColorValue);
  }

  Future<void> dispose() async {
    _disposed = true;
    _opSerial++;
    _hashesByFileId.clear();
    analysisProcesses.analysisIpcPort = null;
    analysisProcesses.analysisIpcToken = null;
    await _ipcServer.dispose();
  }

  Future<void> triggerAnalysis() {
    return _enqueueOperation(_triggerAnalysisImpl);
  }

  Future<void> toggleOverlay(TrackEntry track, String hash) {
    return _enqueueOperation(
      (serial) => _toggleOverlayImpl(serial, track, hash),
    );
  }

  Future<void> toggleOverlayForSlot(int slotIndex) {
    return _enqueueOperation((serial) async {
      if (slotIndex < 0 || slotIndex >= trackManager.entries.length) return;
      final track = trackManager.entries[slotIndex];
      final activeHash = analysisGeneration.activeOverlayHash;
      final knownHash = _hashesByFileId[track.fileId];
      if (activeHash != null && knownHash == activeHash) {
        analysisGeneration.deactivateOverlay();
        return;
      }
      final hash = await analysisGeneration.ensureGenerated(track.path);
      if (!_alive(serial) || hash == null) return;
      await _toggleOverlayImpl(serial, track, hash);
    });
  }

  void updateOverlayConfig(AnalysisOverlayConfig config) {
    if (_disposed) return;
    analysisGeneration.updateOverlayConfig(config);
  }

  void deactivateOverlay() {
    if (_disposed) return;
    analysisGeneration.deactivateOverlay();
  }

  Future<void> _triggerAnalysisImpl(int serial) async {
    if (trackManager.isEmpty) return;
    if (analysisProcesses.activateAnalysisWindows()) return;
    final windows = <AnalysisWindowRequest>[];
    await _ipcServer.start();
    if (!_alive(serial)) return;
    _ipcServer.publishAccentColor(analysisProcesses.accentColorValue);
    analysisProcesses.analysisIpcPort = _ipcServer.port;
    analysisProcesses.analysisIpcToken = _ipcServer.token;
    for (final entry in trackManager.entries) {
      final hash = await analysisGeneration.ensureGenerated(entry.path);
      if (!_alive(serial)) return;
      if (hash != null) {
        _hashesByFileId[entry.fileId] = hash;
        windows.add((hash: hash, fileName: p.basename(entry.path)));
      }
    }
    await _publishTrackSnapshotImpl(serial);
    if (!_alive(serial)) return;
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

  Future<void> _toggleOverlayImpl(
    int serial,
    TrackEntry track,
    String hash,
  ) async {
    final stillOpen = trackManager.entries.any(
      (entry) => entry.fileId == track.fileId && entry.path == track.path,
    );
    if (!stillOpen) return;
    if (analysisGeneration.activeOverlayHash == hash) {
      analysisGeneration.deactivateOverlay();
      return;
    }
    final activated = await analysisGeneration.activateOverlay(
      hash,
      name: track.fileName,
      path: track.path,
    );
    if (!_alive(serial) || !activated) return;
    _hashesByFileId[track.fileId] = hash;
  }

  Future<void> _publishTrackSnapshotImpl(int serial) async {
    if (!_ipcServer.isStarted) return;
    if (!_ipcServer.hasClients) return;
    final tracks = <AnalysisIpcTrack>[];
    final liveFileIds = trackManager.entries.map((e) => e.fileId).toSet();
    _hashesByFileId.removeWhere((fileId, _) => !liveFileIds.contains(fileId));

    for (final entry in trackManager.entries) {
      var hash = _hashesByFileId[entry.fileId];
      if (hash == null) {
        hash = await analysisGeneration.ensureGenerated(entry.path);
        if (!_alive(serial)) return;
        if (hash == null) continue;
        _hashesByFileId[entry.fileId] = hash;
      }
      if (!_alive(serial)) return;
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

    if (!_alive(serial)) return;
    _ipcServer.publishTracks(tracks);
  }

  void _handleAnalysisWindowExited() {
    unawaited(_enqueueOperation(_deactivateIpcWorkspace));
  }

  Future<void> _deactivateIpcWorkspace(int serial) async {
    final port = _ipcServer.port;
    final token = _ipcServer.token;
    await _ipcServer.dispose();
    _hashesByFileId.clear();
    if (!_alive(serial)) return;
    if (analysisProcesses.analysisIpcPort == port &&
        analysisProcesses.analysisIpcToken == token) {
      analysisProcesses.analysisIpcPort = null;
      analysisProcesses.analysisIpcToken = null;
    }
  }

  Future<void> _enqueueOperation(Future<void> Function(int serial) operation) {
    if (_disposed) return Future.value();
    final serial = ++_opSerial;
    final previous = _operationInFlight;
    late final Future<void> future;
    future = (previous ?? Future<void>.value())
        .catchError((_) {})
        .then((_) async {
          if (!_alive(serial)) return;
          await operation(serial);
        })
        .whenComplete(() {
          if (identical(_operationInFlight, future)) {
            _operationInFlight = null;
          }
        });
    _operationInFlight = future;
    return future;
  }

  bool _alive(int serial) => !_disposed && serial == _opSerial;
}
