import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_ffi.dart';
import 'package:void_player/main_window/main_window_quick_marks.dart';
import 'package:void_player/marks/quick_mark.dart';

void main() {
  test('quality metric marks include provenance and skip existing samples', () {
    final existing = QuickMark(
      id: 9,
      anchor: const QuickMarkAnchor(fileId: 7, ptsUs: 1000000, dtsUs: 1000000),
      sourceRect: const Rect.fromLTWH(0, 0, 1, 1),
      origin: QuickMarkOrigin.metric,
      attributes: const {'metric': 'blockiness'},
    );

    final marks = buildQualityMetricMarks(
      existingMarks: [existing],
      fileId: 7,
      metric: AnalysisQualityMetric.blockiness,
      threshold: 0.3,
      report: _report,
    );

    expect(marks, hasLength(1));
    final mark = marks.single;
    expect(mark.ptsUs, 2000000);
    expect(mark.anchor.analysisFrameIndex, -1);
    expect(mark.origin, QuickMarkOrigin.metric);
    expect(mark.defectType, QuickMarkDefectTypes.blocking);
    expect(mark.syncAcrossTracks, isFalse);
    expect(mark.attributes, {
      'metric': 'blockiness',
      'value': 0.4,
      'threshold': 0.3,
      'sampleIndex': 2,
      'decodedFrameIndex': 60,
      'schemaVersion': 4,
    });
  });
}

const _report = AnalysisQualityReport(
  schemaVersion: 4,
  videoWidth: 1920,
  videoHeight: 1080,
  bitDepth: 8,
  sampleIntervalUs: 1000000,
  maxSamples: 600,
  truncated: false,
  unsupportedPixelFrames: 0,
  distributions: {},
  samples: [
    AnalysisQualitySample(
      sampleIndex: 0,
      decodedFrameIndex: 0,
      ptsUs: 0,
      blockiness: 0.1,
      banding: 0.1,
      blur: 0.1,
      noise: 0.1,
      flicker: null,
      averageQp: null,
    ),
    AnalysisQualitySample(
      sampleIndex: 1,
      decodedFrameIndex: 30,
      ptsUs: 1000000,
      blockiness: 0.3,
      banding: 0.1,
      blur: 0.1,
      noise: 0.1,
      flicker: 0.1,
      averageQp: null,
    ),
    AnalysisQualitySample(
      sampleIndex: 2,
      decodedFrameIndex: 60,
      ptsUs: 2000000,
      blockiness: 0.4,
      banding: 0.2,
      blur: 0.2,
      noise: 0.2,
      flicker: 0.2,
      averageQp: null,
    ),
  ],
);
