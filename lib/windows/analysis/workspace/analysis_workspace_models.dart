import '../ipc/analysis_ipc_models.dart';

class AnalysisWorkspaceEntry {
  final String hash;
  final String? fileName;

  const AnalysisWorkspaceEntry({required this.hash, this.fileName});

  factory AnalysisWorkspaceEntry.fromIpcTrack(AnalysisIpcTrack track) =>
      AnalysisWorkspaceEntry(hash: track.hash, fileName: track.fileName);
}
