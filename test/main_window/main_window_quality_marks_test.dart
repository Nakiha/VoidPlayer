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
    expect(mark.sourceRect, const Rect.fromLTWH(0.015, 0.015, 0.97, 0.97));
    expect(mark.attributes, {
      'metric': 'blockiness',
      'value': 0.4,
      'threshold': 0.3,
      'sampleIndex': 2,
      'decodedFrameIndex': 60,
      'schemaVersion': 4,
      'scope': 'frame',
    });
  });

  test('CLI reports generate marks from candidate events, not threshold', () {
    final marks = buildQualityMetricMarks(
      existingMarks: const [],
      fileId: 7,
      metric: AnalysisQualityMetric.banding,
      threshold: 0.99,
      report: _eventReport,
    );

    expect(marks, hasLength(2));

    final spatial = marks.first;
    expect(spatial.ptsUs, 4000000);
    expect(spatial.sourceRect, const Rect.fromLTWH(0.1, 0.2, 0.3, 0.4));
    expect(spatial.isTimeOnly, isFalse);
    expect(spatial.attributes, {
      'metric': 'banding',
      'value': 0.62,
      'threshold': 0.5,
      'eventId': 'evt-spatial',
      'resultKey': 'fnv1a64:abc123',
      'classification': 'spatialCandidate',
      'startPtsUs': 3000000,
      'endPtsUs': 5000000,
      'schemaVersion': 5,
      'scope': 'region',
    });

    final relative = marks.last;
    expect(relative.ptsUs, 8000000);
    expect(relative.isTimeOnly, isFalse);
    expect(relative.sourceRect, const Rect.fromLTWH(0, 0.5, 0.5, 0.5));
    expect(relative.attributes['scope'], 'tile');
    expect(relative.attributes['classification'], 'relativeOutlier');
    expect(relative.attributes['tileColumn'], 0);
    expect(relative.attributes['tileRow'], 1);
    expect(relative.attributes['tileValue'], 0.9);
  });

  test('CLI event marks dedupe by report identity and event id', () {
    final existing = QuickMark(
      id: 3,
      anchor: const QuickMarkAnchor(fileId: 7, ptsUs: 4000000, dtsUs: 4000000),
      sourceRect: const Rect.fromLTWH(0.1, 0.2, 0.3, 0.4),
      origin: QuickMarkOrigin.metric,
      attributes: const {
        'metric': 'banding',
        'eventId': 'evt-spatial',
        'resultKey': 'fnv1a64:abc123',
      },
    );

    final bandingMarks = buildQualityMetricMarks(
      existingMarks: [existing],
      fileId: 7,
      metric: AnalysisQualityMetric.banding,
      threshold: 0.1,
      report: _eventReport,
    );
    expect(bandingMarks, hasLength(1));
    expect(bandingMarks.single.attributes['eventId'], 'evt-relative');

    final blockinessMarks = buildQualityMetricMarks(
      existingMarks: const [],
      fileId: 7,
      metric: AnalysisQualityMetric.blockiness,
      threshold: 0.1,
      report: _eventReport,
    );
    expect(blockinessMarks, isEmpty);
  });

  test('CLI event ids may be reused by a different report', () {
    final existing = QuickMark(
      id: 3,
      anchor: const QuickMarkAnchor(fileId: 7, ptsUs: 4000000, dtsUs: 4000000),
      sourceRect: const Rect.fromLTWH(0.1, 0.2, 0.3, 0.4),
      origin: QuickMarkOrigin.metric,
      attributes: const {
        'metric': 'banding',
        'eventId': 'evt-spatial',
        'resultKey': 'fnv1a64:older-report',
      },
    );

    final marks = buildQualityMetricMarks(
      existingMarks: [existing],
      fileId: 7,
      metric: AnalysisQualityMetric.banding,
      threshold: 0.1,
      report: _eventReport,
    );

    expect(marks, hasLength(2));
    expect(
      marks.map((mark) => mark.attributes['resultKey']),
      everyElement(_eventReport.resultKey),
    );
  });

  test('CLI event marks preserve multiple regions at the same timestamp', () {
    final report = AnalysisQualityReport(
      schemaVersion: _eventReport.schemaVersion,
      metricVersion: _eventReport.metricVersion,
      resultKey: _eventReport.resultKey,
      videoWidth: _eventReport.videoWidth,
      videoHeight: _eventReport.videoHeight,
      bitDepth: _eventReport.bitDepth,
      sampleIntervalUs: _eventReport.sampleIntervalUs,
      maxSamples: _eventReport.maxSamples,
      truncated: false,
      unsupportedPixelFrames: 0,
      distributions: const {},
      samples: const [],
      events: [
        _eventReport.events.first,
        const AnalysisQualityEvent(
          eventId: 'evt-spatial-2',
          metric: AnalysisQualityMetric.banding,
          classification: AnalysisQualityEventClassification.spatialCandidate,
          startPtsUs: 4000000,
          endPtsUs: 4000000,
          peakPtsUs: 4000000,
          startSampleIndex: 4,
          endSampleIndex: 4,
          peakSampleIndex: 4,
          peakScore: 0.03,
          evidenceSampleCount: 1,
          threshold: 0.5,
          region: AnalysisQualitySpatialRegion(
            score: 0.72,
            detectionThreshold: 0.5,
            x: 0.65,
            y: 0.2,
            width: 0.25,
            height: 0.4,
            pixelX: 1248,
            pixelY: 216,
            pixelWidth: 480,
            pixelHeight: 432,
          ),
        ),
      ],
    );

    final marks = buildQualityMetricMarks(
      existingMarks: const [],
      fileId: 7,
      metric: AnalysisQualityMetric.banding,
      threshold: 1,
      report: report,
    );

    expect(marks, hasLength(2));
    expect(marks.map((mark) => mark.ptsUs), everyElement(4000000));
    expect(marks.last.attributes['value'], 0.72);
    expect(marks.last.sourceRect, const Rect.fromLTWH(0.65, 0.2, 0.25, 0.4));
  });
}

const _report = AnalysisQualityReport(
  schemaVersion: 4,
  metricVersion: '',
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

const _eventReport = AnalysisQualityReport(
  schemaVersion: 5,
  metricVersion: 'quality-demo-v5',
  resultKey: 'fnv1a64:abc123',
  videoWidth: 1920,
  videoHeight: 1080,
  bitDepth: 8,
  sampleIntervalUs: 1000000,
  maxSamples: 600,
  truncated: false,
  unsupportedPixelFrames: 0,
  distributions: {},
  samples: [],
  tileSamples: [
    AnalysisQualityTileSample(
      sampleIndex: 8,
      decodedFrameIndex: 240,
      ptsUs: 8000000,
      tileMetricVersion: 'quality-tile-metrics-v1',
      frameWidth: 1920,
      frameHeight: 1080,
      targetTileWidth: 64,
      targetTileHeight: 64,
      columns: 2,
      rows: 2,
      metrics: {
        AnalysisQualityMetric.banding: AnalysisQualityTileMetricData(
          available: true,
          algorithm: 'banding-proxy-local-v1',
          values: [0.1, 0.2, 0.9, 0.3],
        ),
      },
    ),
  ],
  events: [
    AnalysisQualityEvent(
      eventId: 'evt-spatial',
      metric: AnalysisQualityMetric.banding,
      classification: AnalysisQualityEventClassification.spatialCandidate,
      startPtsUs: 3000000,
      endPtsUs: 5000000,
      peakPtsUs: 4000000,
      startSampleIndex: 3,
      endSampleIndex: 5,
      peakSampleIndex: 4,
      peakScore: 0.62,
      evidenceSampleCount: 3,
      threshold: 0.5,
      region: AnalysisQualitySpatialRegion(
        score: 0.62,
        detectionThreshold: 0.5,
        x: 0.1,
        y: 0.2,
        width: 0.3,
        height: 0.4,
        pixelX: 192,
        pixelY: 216,
        pixelWidth: 576,
        pixelHeight: 432,
      ),
    ),
    AnalysisQualityEvent(
      eventId: 'evt-relative',
      metric: AnalysisQualityMetric.banding,
      classification: AnalysisQualityEventClassification.relativeOutlier,
      startPtsUs: 7000000,
      endPtsUs: 9000000,
      peakPtsUs: 8000000,
      startSampleIndex: 7,
      endSampleIndex: 9,
      peakSampleIndex: 8,
      peakScore: 0.55,
      evidenceSampleCount: 6,
      threshold: 0.4,
      region: null,
    ),
  ],
);
