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
