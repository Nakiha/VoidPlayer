import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/performance/performance_health.dart';

void main() {
  test('classifies empty diagnostics as healthy', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({});

    expect(snapshot.level, PerformanceHealthLevel.ok);
    expect(snapshot.kind, PerformanceHealthKind.ok);
  });

  test('classifies native render pressure from renderer latency', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'displayRefreshHzEstimate': 120.0,
      'displayTickHz': 118.0,
      'nativeRendererDrawP95Us': 12000.0,
    });

    expect(snapshot.level, PerformanceHealthLevel.warning);
    expect(snapshot.kind, PerformanceHealthKind.nativeRenderPressure);
    expect(snapshot.reason, 'native-render');
  });

  test(
    'classifies display pressure when ticks fall behind without native slowness',
    () {
      final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
        'trackCount': 1,
        'displayRefreshHzEstimate': 120.0,
        'displayTickHz': 70.0,
        'layoutIntentHz': 100.0,
        'layoutDrawHz': 65.0,
        'nativeRendererDrawP95Us': 3000.0,
        'nativeRendererDrawBackendP95Us': 2500.0,
        'metalCommandCompletionP95Us': 3000.0,
      });

      expect(snapshot.level, PerformanceHealthLevel.warning);
      expect(snapshot.kind, PerformanceHealthKind.externalDisplayPressure);
      expect(snapshot.reason, 'display-pressure');
    },
  );

  test('classifies decode buffer pressure from track diagnostics', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'nativeTrackDiagnostics': [
        {'bufferState': 1, 'bufferCount': 0, 'bufferCapacity': 8},
      ],
    });

    expect(snapshot.level, PerformanceHealthLevel.warning);
    expect(snapshot.kind, PerformanceHealthKind.decodePressure);
    expect(snapshot.reason, 'decode-buffer');
  });

  test(
    'classifies playback cadence pressure when presented fps trails media',
    () {
      final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
        'trackCount': 1,
        'isPlaying': true,
        'presentedFramePtsDistinctCount': 131,
        'presentedFramePtsAdvanceUs': 6030000,
        'presentedFrameExpectedIntervalUs': 33333,
        'presentedFramePtsLargeGapCount': 0,
        'presentedFramePtsMonotonicViolationCount': 0,
        'nativeRendererDrawP95Us': 3500.0,
        'nativeRendererDrawBackendP95Us': 3200.0,
        'metalCommandCompletionP95Us': 3200.0,
      });

      expect(snapshot.level, PerformanceHealthLevel.warning);
      expect(snapshot.kind, PerformanceHealthKind.playbackCadencePressure);
      expect(snapshot.reason, 'playback-cadence');
      expect(snapshot.presentedFrameRateHz, closeTo(21.6, 0.2));
      expect(snapshot.expectedFrameRateHz, closeTo(30.0, 0.1));
      expect(snapshot.playbackCadenceRatio, lessThan(0.82));
    },
  );

  test('keeps healthy playback cadence near the media frame rate', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'presentedFramePtsDistinctCount': 179,
      'presentedFramePtsAdvanceUs': 6030000,
      'presentedFrameExpectedIntervalUs': 33333,
      'nativeRendererDrawP95Us': 3500.0,
      'nativeRendererDrawBackendP95Us': 3200.0,
      'metalCommandCompletionP95Us': 3200.0,
    });

    expect(snapshot.level, PerformanceHealthLevel.ok);
    expect(snapshot.kind, PerformanceHealthKind.ok);
    expect(snapshot.playbackCadenceRatio, greaterThan(0.95));
  });

  test('uses counter deltas for ring pressure', () {
    final previous = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'metalBufferExhaustionCount': 4,
    });
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'metalBufferExhaustionCount': 5,
    }, previous: previous);

    expect(snapshot.level, PerformanceHealthLevel.severe);
    expect(snapshot.kind, PerformanceHealthKind.nativeRenderPressure);
    expect(snapshot.metalBufferExhaustionDelta, 1);
  });

  test('does not warn from static paused overlay redraw latency', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': false,
      'layoutIntentHz': 0.0,
      'nativeRendererDrawP95Us': 18000.0,
      'nativeRendererDrawBackendP95Us': 16000.0,
      'metalCommandCompletionP95Us': 17000.0,
    });

    expect(snapshot.level, PerformanceHealthLevel.ok);
    expect(snapshot.kind, PerformanceHealthKind.ok);
  });

  testWidgets('hides large-gap details while paused', (tester) async {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': false,
      'presentedFramePtsLargeGapCount': 2,
    });
    late String detail;

    await tester.pumpWidget(
      MaterialApp(
        locale: const Locale('en'),
        home: Builder(
          builder: (context) {
            detail = snapshot.localizedDetail(context);
            return const SizedBox.shrink();
          },
        ),
      ),
    );

    expect(snapshot.level, PerformanceHealthLevel.ok);
    expect(detail, isNot(contains('gap')));
  });

  testWidgets('keeps cadence out of summary detail while playing', (
    tester,
  ) async {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'presentedFramePtsDistinctCount': 131,
      'presentedFramePtsAdvanceUs': 6030000,
      'presentedFrameExpectedIntervalUs': 33333,
    });
    late String detail;

    await tester.pumpWidget(
      MaterialApp(
        locale: const Locale('en'),
        home: Builder(
          builder: (context) {
            detail = snapshot.localizedDetail(context);
            return const SizedBox.shrink();
          },
        ),
      ),
    );

    expect(detail, isNot(contains('cadence')));
    expect(detail, isNot(contains('fps')));
  });

  testWidgets('shows idle display sampling without reporting 0Hz', (
    tester,
  ) async {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'displayRefreshHzEstimate': 121.0,
      'displayTickHz': 0.0,
    });
    late String detail;

    await tester.pumpWidget(
      MaterialApp(
        locale: const Locale('en'),
        home: Builder(
          builder: (context) {
            detail = snapshot.localizedDetail(context);
            return const SizedBox.shrink();
          },
        ),
      ),
    );

    expect(detail, contains('display idle/121Hz'));
    expect(detail, isNot(contains('display 0/121Hz')));
  });

  testWidgets('keeps layout sampling inside the display summary', (
    tester,
  ) async {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'displayRefreshHzEstimate': 120.0,
      'displayTickHz': 120.0,
      'layoutDrawHz': 92.0,
      'nativeRendererDrawP95Us': 1800.0,
    });
    late String detail;

    await tester.pumpWidget(
      MaterialApp(
        locale: const Locale('en'),
        home: Builder(
          builder: (context) {
            detail = snapshot.localizedDetail(context);
            return const SizedBox.shrink();
          },
        ),
      ),
    );

    expect(detail, contains('display 120/120Hz'));
    expect(detail, isNot(contains('layout')));
  });

  testWidgets('display pressure feedback avoids environment-specific advice', (
    tester,
  ) async {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'displayRefreshHzEstimate': 120.0,
      'displayTickHz': 70.0,
      'layoutIntentHz': 100.0,
      'layoutDrawHz': 65.0,
    });
    late String feedback;

    await tester.pumpWidget(
      MaterialApp(
        locale: const Locale('en'),
        home: Builder(
          builder: (context) {
            feedback = snapshot.localizedFeedback(context);
            return const SizedBox.shrink();
          },
        ),
      ),
    );

    expect(feedback, isNot(contains('HDR')));
    expect(feedback, contains('external displays'));
    expect(feedback, contains('screen recording'));
  });
}
