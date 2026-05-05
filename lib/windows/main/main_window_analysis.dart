import 'dart:async';

import 'package:path/path.dart' as p;

import '../../analysis/analysis_manager.dart';
import '../../track_manager.dart';
import '../analysis/ipc/analysis_ipc_models.dart';
import '../analysis/ipc/analysis_ipc_server.dart';
import '../window_manager.dart';

class MainWindowAnalysisCoordinator {
  final TrackManager trackManager;
  final AnalysisProcessManager analysisProcesses;
  final AnalysisIpcServer _ipcServer = AnalysisIpcServer();
  final Map<int, String> _hashesByFileId = <int, String>{};

  int _opSerial = 0;
  bool _disposed = false;
  Future<void>? _operationInFlight;

  MainWindowAnalysisCoordinator({
    required this.trackManager,
    required this.analysisProcesses,
  });

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

  Future<void> _triggerAnalysisImpl(int serial) async {
    if (trackManager.isEmpty) return;
    if (analysisProcesses.activateAnalysisWindows()) return;
    final mgr = AnalysisManager.instance;
    final windows = <AnalysisWindowRequest>[];
    await _ipcServer.start();
    if (!_alive(serial)) return;
    analysisProcesses.analysisIpcPort = _ipcServer.port;
    analysisProcesses.analysisIpcToken = _ipcServer.token;
    for (final entry in trackManager.entries) {
      final hash = await mgr.ensureGenerated(entry.path);
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

  Future<void> _publishTrackSnapshotImpl(int serial) async {
    if (!_ipcServer.isStarted) return;
    if (!_ipcServer.hasClients) return;
    final mgr = AnalysisManager.instance;
    final tracks = <AnalysisIpcTrack>[];
    final liveFileIds = trackManager.entries.map((e) => e.fileId).toSet();
    _hashesByFileId.removeWhere((fileId, _) => !liveFileIds.contains(fileId));

    for (final entry in trackManager.entries) {
      var hash = _hashesByFileId[entry.fileId];
      if (hash == null) {
        hash = await mgr.ensureGenerated(entry.path);
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
