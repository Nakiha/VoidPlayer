import '../analysis_ffi.dart';
import '../nalu_types.dart';

sealed class AnalysisUiSelection {
  final int fileId;
  final AnalysisCodec codec;

  const AnalysisUiSelection({required this.fileId, required this.codec});

  Object get identity;
}

final class AnalysisFrameSelection extends AnalysisUiSelection {
  final int frameIndex;
  final FrameInfo frame;

  const AnalysisFrameSelection({
    required super.fileId,
    required super.codec,
    required this.frameIndex,
    required this.frame,
  });

  @override
  Object get identity => (AnalysisFrameSelection, fileId, frameIndex);
}

final class AnalysisNaluSelection extends AnalysisUiSelection {
  final int naluIndex;
  final NaluInfo nalu;
  final int? frameIndex;
  final FrameInfo? frame;

  const AnalysisNaluSelection({
    required super.fileId,
    required super.codec,
    required this.naluIndex,
    required this.nalu,
    this.frameIndex,
    this.frame,
  });

  @override
  Object get identity => (AnalysisNaluSelection, fileId, naluIndex);
}

final class AnalysisFrameSeekRequest {
  final int fileId;
  final int frameIndex;
  final int trackPtsUs;

  const AnalysisFrameSeekRequest({
    required this.fileId,
    required this.frameIndex,
    required this.trackPtsUs,
  });
}

final class AnalysisPlaybackPosition {
  final int ptsUs;
  final int dtsUs;
  final int analysisFrameIndex;

  const AnalysisPlaybackPosition({
    required this.ptsUs,
    required this.dtsUs,
    required this.analysisFrameIndex,
  });

  @override
  bool operator ==(Object other) =>
      other is AnalysisPlaybackPosition &&
      other.ptsUs == ptsUs &&
      other.dtsUs == dtsUs &&
      other.analysisFrameIndex == analysisFrameIndex;

  @override
  int get hashCode => Object.hash(ptsUs, dtsUs, analysisFrameIndex);
}

int? analysisFramePtsUs(FrameInfo frame, AnalysisSummary? summary) {
  if (summary == null || summary.timeBaseNum <= 0 || summary.timeBaseDen <= 0) {
    return null;
  }
  const noTimestamp = -9223372036854775808;
  final timestamp = frame.pts != noTimestamp ? frame.pts : frame.dts;
  if (timestamp == noTimestamp) return null;
  final numerator = timestamp * summary.timeBaseNum * 1000000;
  final halfDenominator = summary.timeBaseDen ~/ 2;
  return numerator >= 0
      ? (numerator + halfDenominator) ~/ summary.timeBaseDen
      : -((-numerator + halfDenominator) ~/ summary.timeBaseDen);
}
