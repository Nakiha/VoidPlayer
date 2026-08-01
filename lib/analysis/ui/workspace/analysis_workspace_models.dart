import '../../analysis_manager.dart';

class AnalysisWorkspaceEntry {
  final int fileId;
  final String path;
  final String fileName;
  final String? hash;
  final AnalysisTrackGenerationStatus? generationStatus;

  const AnalysisWorkspaceEntry({
    required this.fileId,
    required this.path,
    required this.fileName,
    required this.hash,
    required this.generationStatus,
  });

  bool get isReady => hash?.isNotEmpty ?? false;

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is AnalysisWorkspaceEntry &&
          other.fileId == fileId &&
          other.path == path &&
          other.fileName == fileName &&
          other.hash == hash &&
          identical(other.generationStatus, generationStatus);

  @override
  int get hashCode => Object.hash(
    fileId,
    path,
    fileName,
    hash,
    identityHashCode(generationStatus),
  );
}
