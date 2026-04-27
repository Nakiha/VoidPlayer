part of 'main_window.dart';

extension _MainWindowAnalysis on _MainWindowState {
  Future<void> _triggerAnalysis() async {
    if (_trackManager.isEmpty) return;
    final mgr = AnalysisManager.instance;
    final windows = <AnalysisWindowRequest>[];
    await _analysisIpcServer.start();
    WindowManager.analysisIpcPort = _analysisIpcServer.port;
    WindowManager.analysisIpcToken = _analysisIpcServer.token;
    for (final entry in _trackManager.entries) {
      final hash = await mgr.ensureAndLoad(entry.path);
      if (hash != null) {
        _analysisHashesByFileId[entry.fileId] = hash;
        windows.add((hash: hash, fileName: p.basename(entry.path)));
      }
    }
    await _publishAnalysisTrackSnapshot();
    await WindowManager.showAnalysisWindows(windows);
  }

  Future<void> _publishAnalysisTrackSnapshot() async {
    if (!_analysisIpcServer.isStarted) return;
    final serial = ++_analysisSnapshotSerial;
    final mgr = AnalysisManager.instance;
    final tracks = <AnalysisIpcTrack>[];
    final liveFileIds = _trackManager.entries.map((e) => e.fileId).toSet();
    _analysisHashesByFileId.removeWhere(
      (fileId, _) => !liveFileIds.contains(fileId),
    );

    for (final entry in _trackManager.entries) {
      var hash = _analysisHashesByFileId[entry.fileId];
      if (hash == null) {
        hash = await mgr.ensureAndLoad(entry.path);
        if (hash == null) continue;
        _analysisHashesByFileId[entry.fileId] = hash;
      }
      if (serial != _analysisSnapshotSerial) return;
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

    if (serial != _analysisSnapshotSerial) return;
    _analysisIpcServer.publishTracks(tracks);
  }
}
