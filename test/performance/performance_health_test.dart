import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/performance/performance_health.dart';
import 'package:void_player/preferences/playback_preferences.dart';

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
      'nativeRendererDrawBackendP95Us': 11000.0,
    });

    expect(snapshot.level, PerformanceHealthLevel.warning);
    expect(snapshot.kind, PerformanceHealthKind.nativeRenderPressure);
    expect(snapshot.reason, 'native-render');
  });

  test('uses retained WGPU timings for source-provider native pressure', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'displayRefreshHzEstimate': 120.0,
      'presentationBackend': 'native-wgpu-metal-source-provider',
      'nativeCompositorBackend': 'wgpu-metal-thin-runner',
      'nativeCompositorCompositeHz': 120.0,
      'nativeRendererDrawP95Us': 12000.0,
      'nativeRendererDrawBackendP95Us': 11000.0,
      'metalCommandCompletionP95Us': 10500.0,
      'nativeCompositorFrameCpuP95Ms': 0.28,
      'nativeCompositorWgpuSubmitCpuP95Ms': 0.19,
      'nativeCompositorWgpuCompletionP95Ms': 3.85,
    });

    expect(snapshot.level, PerformanceHealthLevel.ok);
    expect(snapshot.kind, PerformanceHealthKind.ok);
    expect(snapshot.drawP95Us, closeTo(280.0, 0.1));
    expect(snapshot.backendP95Us, closeTo(190.0, 0.1));
    expect(snapshot.metalP95Us, closeTo(3850.0, 0.1));
  });

  test('classifies queued GPU completion latency as display pressure', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 2,
      'isPlaying': false,
      'displayRefreshHzEstimate': 120.0,
      'displayTickHz': 120.0,
      'layoutIntentHz': 61.6,
      'layoutDrawHz': 19.8,
      'nativeCompositorCompositeHz': 103.3,
      'nativeRendererDrawP95Us': 64200.0,
      'nativeRendererDrawBackendP95Us': 6100.0,
      'metalCommandCompletionP95Us': 6100.0,
    });

    expect(snapshot.level, PerformanceHealthLevel.severe);
    expect(snapshot.kind, PerformanceHealthKind.externalDisplayPressure);
    expect(snapshot.reason, 'display-pressure');
    expect(snapshot.diagnosticSummary, contains('gpu-completion-high'));
    expect(snapshot.diagnosticSummary, isNot(contains('reason=native-render')));
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

  test('uses native compositor composite rate for layout display pressure', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'displayRefreshHzEstimate': 120.0,
      'displayTickHz': 120.0,
      'layoutIntentHz': 120.0,
      'layoutDrawHz': 55.0,
      'nativeCompositorEnabled': true,
      'nativeCompositorCompositeHz': 118.0,
      'nativeRendererDrawP95Us': 3000.0,
      'nativeRendererDrawBackendP95Us': 2500.0,
      'metalCommandCompletionP95Us': 3000.0,
    });

    expect(snapshot.level, PerformanceHealthLevel.ok);
    expect(snapshot.kind, PerformanceHealthKind.ok);
    expect(snapshot.diagnosticSummary, contains('compositor=118.0Hz'));
  });

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

  test('ignores empty decode buffers for tracks already past their end', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 2,
      'isPlaying': true,
      'nativeTrackDiagnostics': [
        {
          'bufferState': 0,
          'bufferCount': 4,
          'bufferCapacity': 4,
          'durationUs': 9400000,
          'currentPtsUs': 4014000,
        },
        {
          'bufferState': 0,
          'bufferCount': 0,
          'bufferCapacity': 1,
          'durationUs': 3000000,
          'currentPtsUs': 4014000,
        },
      ],
    });

    expect(snapshot.level, PerformanceHealthLevel.ok);
    expect(snapshot.kind, PerformanceHealthKind.ok);
  });

  test('keeps empty decode buffers as pressure before track end', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'nativeTrackDiagnostics': [
        {
          'bufferState': 0,
          'bufferCount': 0,
          'bufferCapacity': 1,
          'durationUs': 9400000,
          'currentPtsUs': 4014000,
        },
      ],
    });

    expect(snapshot.level, PerformanceHealthLevel.warning);
    expect(snapshot.kind, PerformanceHealthKind.decodePressure);
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

  test('keeps host interval spikes healthy when cadence is stable', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'displayRefreshHzEstimate': 120.0,
      'displayTickHz': 118.0,
      'layoutIntentHz': 58.5,
      'layoutDrawHz': 76.7,
      'presentedFrameHostIntervalP95Ms': 396.0,
      'presentedFramePtsDistinctCount': 179,
      'presentedFramePtsAdvanceUs': 6030000,
      'presentedFrameExpectedIntervalUs': 33333,
      'nativeRendererDrawP95Us': 2200.0,
      'nativeRendererDrawBackendP95Us': 2100.0,
      'metalCommandCompletionP95Us': 2000.0,
    });

    expect(snapshot.level, PerformanceHealthLevel.ok);
    expect(snapshot.kind, PerformanceHealthKind.ok);
    expect(snapshot.playbackCadenceRatio, greaterThan(0.95));
    expect(snapshot.diagnosticSummary, contains('host-interval-high'));
  });

  test('keeps normal 30fps media healthy on a high refresh display', () {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'displayRefreshHzEstimate': 120.0,
      'displayTickHz': 0.0,
      'presentedFrameHostIntervalP95Ms': 34.0,
      'presentedFramePtsDistinctCount': 179,
      'presentedFramePtsAdvanceUs': 6030000,
      'presentedFrameExpectedIntervalUs': 33333,
      'nativeRendererDrawP95Us': 2800.0,
      'nativeRendererDrawBackendP95Us': 2600.0,
      'metalCommandCompletionP95Us': 2800.0,
    });

    expect(snapshot.level, PerformanceHealthLevel.ok);
    expect(snapshot.kind, PerformanceHealthKind.ok);
  });

  test('classifies PTS timeline gaps as playback cadence pressure', () {
    final previous = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'presentedFramePtsLargeGapCount': 0,
    });
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'presentedFramePtsLargeGapCount': 1,
      'nativeRendererDrawP95Us': 3000.0,
      'nativeRendererDrawBackendP95Us': 2500.0,
      'metalCommandCompletionP95Us': 3000.0,
    }, previous: previous);

    expect(snapshot.level, PerformanceHealthLevel.warning);
    expect(snapshot.kind, PerformanceHealthKind.playbackCadencePressure);
    expect(snapshot.reason, 'playback-cadence');
  });

  test(
    'classifies PTS monotonic violations as severe playback cadence pressure',
    () {
      final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
        'trackCount': 1,
        'isPlaying': true,
        'presentedFramePtsMonotonicViolationCount': 1,
        'nativeRendererDrawP95Us': 3000.0,
        'nativeRendererDrawBackendP95Us': 2500.0,
        'metalCommandCompletionP95Us': 3000.0,
      });

      expect(snapshot.level, PerformanceHealthLevel.severe);
      expect(snapshot.kind, PerformanceHealthKind.playbackCadencePressure);
      expect(snapshot.reason, 'playback-cadence');
    },
  );

  test('keeps isolated ring exhaustion healthy', () {
    final previous = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'metalBufferExhaustionCount': 4,
    });
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'metalBufferExhaustionCount': 5,
    }, previous: previous);

    expect(snapshot.level, PerformanceHealthLevel.ok);
    expect(snapshot.kind, PerformanceHealthKind.ok);
    expect(snapshot.metalBufferExhaustionDelta, 1);
    expect(snapshot.diagnosticSummary, contains('ring=5(+1)'));
  });

  test('uses counter deltas for metal failure pressure', () {
    final previous = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'metalCommandFailureCount': 4,
    });
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'metalCommandFailureCount': 5,
    }, previous: previous);

    expect(snapshot.level, PerformanceHealthLevel.severe);
    expect(snapshot.kind, PerformanceHealthKind.nativeRenderPressure);
    expect(snapshot.metalFailureDelta, 1);
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

  testWidgets('shows idle display-link sampling without reporting 0Hz', (
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

    expect(detail, contains('display-link idle/121Hz'));
    expect(detail, isNot(contains('display-link 0/121Hz')));
  });

  testWidgets('keeps layout sampling out of the health detail', (tester) async {
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

    expect(detail, contains('display-link 120/120Hz'));
    expect(detail, isNot(contains('layout')));
  });

  testWidgets('shows native compositor cadence before display-link cadence', (
    tester,
  ) async {
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'displayRefreshHzEstimate': 120.0,
      'displayTickHz': 0.0,
      'nativeCompositorCompositeHz': 86.4,
      'nativeCompositorSourceCacheHz': 29.7,
      'nativeCompositorSourceProjectionHz': 119.1,
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

    expect(detail, startsWith('compositor 86/120Hz'));
    expect(detail, contains('source 30Hz'));
    expect(detail, contains('projection 119Hz'));
    expect(detail, contains('display-link idle/120Hz'));
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

  test('feedback policy requires sustained pressure and respects cooldown', () {
    final policy = PerformanceHealthFeedbackPolicy();
    final now = DateTime(2026, 6, 6, 12);
    final pressure = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'nativeRendererDrawP95Us': 12000.0,
      'nativeRendererDrawBackendP95Us': 11000.0,
    });

    expect(_shouldShow(policy, snapshot: pressure, now: now), isFalse);
    expect(
      _shouldShow(
        policy,
        snapshot: pressure,
        now: now.add(const Duration(milliseconds: 1500)),
      ),
      isFalse,
    );
    expect(
      _shouldShow(
        policy,
        snapshot: pressure,
        now: now.add(const Duration(seconds: 3)),
      ),
      isTrue,
    );
    expect(
      _shouldShow(
        policy,
        snapshot: pressure,
        now: now.add(const Duration(seconds: 10)),
      ),
      isFalse,
    );
    expect(
      _shouldShow(
        policy,
        snapshot: pressure,
        now: now.add(const Duration(seconds: 49)),
      ),
      isTrue,
    );
  });

  test('feedback policy suppresses snackbar while profiler is visible', () {
    final policy = PerformanceHealthFeedbackPolicy();
    final now = DateTime(2026, 6, 6, 12);
    final pressure = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'nativeRendererDrawP95Us': 12000.0,
      'nativeRendererDrawBackendP95Us': 11000.0,
    });

    for (var i = 0; i < 3; i += 1) {
      expect(
        _shouldShow(
          policy,
          snapshot: pressure,
          profilerVisible: true,
          now: now.add(Duration(seconds: i * 2)),
        ),
        isFalse,
      );
    }
    expect(policy.pressureSamples, 3);
  });

  test('feedback policy can show only once per session', () {
    final policy = PerformanceHealthFeedbackPolicy();
    final now = DateTime(2026, 6, 6, 12);
    final pressure = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'nativeRendererDrawP95Us': 12000.0,
      'nativeRendererDrawBackendP95Us': 11000.0,
    });

    for (var i = 0; i < 2; i += 1) {
      expect(
        _shouldShow(
          policy,
          snapshot: pressure,
          alertPolicy: PerformanceAlertPolicy.once,
          now: now.add(Duration(seconds: i)),
        ),
        isFalse,
      );
    }
    expect(
      _shouldShow(
        policy,
        snapshot: pressure,
        alertPolicy: PerformanceAlertPolicy.once,
        now: now.add(const Duration(seconds: 3)),
      ),
      isTrue,
    );
    expect(
      _shouldShow(
        policy,
        snapshot: pressure,
        alertPolicy: PerformanceAlertPolicy.once,
        now: now.add(const Duration(minutes: 2)),
      ),
      isFalse,
    );
  });

  test('feedback policy can be disabled', () {
    final policy = PerformanceHealthFeedbackPolicy();
    final now = DateTime(2026, 6, 6, 12);
    final pressure = PerformanceHealthSnapshot.fromDiagnostics({
      'trackCount': 1,
      'isPlaying': true,
      'nativeRendererDrawP95Us': 12000.0,
      'nativeRendererDrawBackendP95Us': 11000.0,
    });

    for (var i = 0; i < 4; i += 1) {
      expect(
        _shouldShow(
          policy,
          snapshot: pressure,
          alertPolicy: PerformanceAlertPolicy.disabled,
          now: now.add(Duration(seconds: i)),
        ),
        isFalse,
      );
    }
    expect(policy.pressureSamples, 0);
  });
}

bool _shouldShow(
  PerformanceHealthFeedbackPolicy policy, {
  required PerformanceHealthSnapshot snapshot,
  bool profilerVisible = false,
  PerformanceAlertPolicy alertPolicy = PerformanceAlertPolicy.sustained,
  required DateTime now,
}) {
  return policy.shouldShow(
    snapshot: snapshot,
    profilerVisible: profilerVisible,
    alertPolicy: alertPolicy,
    now: now,
  );
}
