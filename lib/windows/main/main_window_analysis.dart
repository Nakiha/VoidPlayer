import 'package:path/path.dart' as p;

import '../../analysis/analysis_manager.dart';
import '../../track_manager.dart';
import '../analysis/ipc/analysis_ipc_models.dart';
import '../analysis/ipc/analysis_ipc_server.dart';
import '../window_manager.dart';

class MainWindowAnalysisCoordinator {
  final TrackManager trackManager;
  final AnalysisIpcServer _ipcServer = AnalysisIpcServer();
  final Map<int, String> _hashesByFileId = <int, String>{};

  int _snapshotSerial = 0;

  MainWindowAnalysisCoordinator({required this.trackManager});

  Future<void> dispose() => _ipcServer.dispose();

  Future<void> triggerAnalysis() async {
    if (trackManager.isEmpty) return;
    final mgr = AnalysisManager.instance;
    final windows = <AnalysisWindowRequest>[];
    await _ipcServer.start();
    WindowManager.analysisIpcPort = _ipcServer.port;
    WindowManager.analysisIpcToken = _ipcServer.token;
    for (final entry in trackManager.entries) {
      final hash = await mgr.ensureAndLoad(entry.path);
      if (hash != null) {
        _hashesByFileId[entry.fileId] = hash;
        windows.add((hash: hash, fileName: p.basename(entry.path)));
      }
    }
    await publishTrackSnapshot();
    await WindowManager.showAnalysisWindows(windows);
  }

  Future<void> publishTrackSnapshot() async {
    if (!_ipcServer.isStarted) return;
    final serial = ++_snapshotSerial;
    final mgr = AnalysisManager.instance;
    final tracks = <AnalysisIpcTrack>[];
    final liveFileIds = trackManager.entries.map((e) => e.fileId).toSet();
    _hashesByFileId.removeWhere((fileId, _) => !liveFileIds.contains(fileId));

    for (final entry in trackManager.entries) {
      var hash = _hashesByFileId[entry.fileId];
      if (hash == null) {
        hash = await mgr.ensureAndLoad(entry.path);
        if (hash == null) continue;
        _hashesByFileId[entry.fileId] = hash;
      }
      if (serial != _snapshotSerial) return;
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

    if (serial != _snapshotSerial) return;
    _ipcServer.publishTracks(tracks);
  }
}
