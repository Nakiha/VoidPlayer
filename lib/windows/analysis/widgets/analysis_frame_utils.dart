import '../../../analysis/analysis_ffi.dart';

String analysisFrameSliceName(FrameInfo frame) => switch (frame.sliceType) {
  2 => 'I',
  1 => 'P',
  _ => frame.numRefL1 > 0 ? 'B' : 'B (uni)',
};
