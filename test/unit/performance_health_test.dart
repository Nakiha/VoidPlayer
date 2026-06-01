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
      'nativeTrackDiagnostics': [
        {'bufferState': 1, 'bufferCount': 0, 'bufferCapacity': 8},
      ],
    });

    expect(snapshot.level, PerformanceHealthLevel.warning);
    expect(snapshot.kind, PerformanceHealthKind.decodePressure);
    expect(snapshot.reason, 'decode-buffer');
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
}
