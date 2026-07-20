import 'package:flutter/material.dart';

import '../../../analysis/analysis_ffi.dart';

const Color analysisIFrameColor = Color(0xFFFF4D4F);
const Color analysisPFrameColor = Color(0xFF52C41A);
const Color analysisBFrameColor = Color(0xFF1890FF);

Color analysisFrameTypeColor(FrameInfo frame) {
  if (frame.sliceType == 2) return analysisIFrameColor;
  if (frame.sliceType == 0 && frame.numRefL1 > 0) {
    return analysisBFrameColor;
  }
  return analysisPFrameColor;
}

Color analysisTemporalLayerColor(int temporalId) {
  const colors = <Color>[
    Color(0xFFFFD54F),
    Color(0xFF26C6DA),
    Color(0xFFAB47BC),
    Color(0xFFFF8A65),
    Color(0xFF9CCC65),
    Color(0xFF5C6BC0),
  ];
  return colors[temporalId.abs() % colors.length];
}
