import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/windows/analysis/charts/analysis_chart_scroll.dart';

void main() {
  test('horizontal scroll intent tolerates small vertical drift', () {
    expect(isChartHorizontalScrollIntent(const Offset(12, 3)), isTrue);
    expect(isChartHorizontalScrollIntent(const Offset(12, 16)), isTrue);
    expect(isChartHorizontalScrollIntent(const Offset(0, 16)), isFalse);
    expect(isChartHorizontalScrollIntent(const Offset(3, 12)), isFalse);
  });

  test('horizontal scroll pans chart opposite to pointer delta', () {
    double? offset;
    final consumed = handleChartHorizontalScrollPan(
      scrollDelta: const Offset(50, 0),
      chartExtent: 500,
      viewStart: 20,
      viewEnd: 120,
      total: 1000,
      onPan: (value) => offset = value,
    );

    expect(consumed, isTrue);
    expect(offset, 10);
  });

  test('negative horizontal scroll delta moves chart window right', () {
    double? offset;
    final consumed = handleChartHorizontalScrollPan(
      scrollDelta: const Offset(-50, 0),
      chartExtent: 500,
      viewStart: 20,
      viewEnd: 120,
      total: 1000,
      onPan: (value) => offset = value,
    );

    expect(consumed, isTrue);
    expect(offset, 30);
  });

  test('horizontal scroll is consumed at bounds without zoom fallback', () {
    double? offset;
    final consumed = handleChartHorizontalScrollPan(
      scrollDelta: const Offset(50, 0),
      chartExtent: 500,
      viewStart: 0,
      viewEnd: 1000,
      total: 1000,
      onPan: (value) => offset = value,
    );

    expect(consumed, isTrue);
    expect(offset, isNull);
  });

  test('frame ticks stay anchored to the same step grid while panning', () {
    final step = stableFrameTickStep(visibleSpan: 8.0, plotExtent: 280);

    expect(step, 3);
    expect(firstStableFrameTick(1.1, step), 0);
    expect(firstStableFrameTick(1.9, step), 0);
    expect(firstStableFrameTick(2.1, step), 0);
    expect(firstStableFrameTick(3.1, step), 3);
  });

  test('scale zoom scroll delta accumulates before firing a zoom step', () {
    var result = accumulateScaleZoomScrollDelta(
      scaleDelta: 1.01,
      accumulator: 0,
    );

    expect(result.scrollDelta, isNull);
    result = accumulateScaleZoomScrollDelta(
      scaleDelta: 1.04,
      accumulator: result.accumulator,
    );

    expect(result.scrollDelta, -1);
    expect(result.accumulator, 0);
  });

  test('pan zoom scale intent ignores pure two finger pan', () {
    expect(isPanZoomScaleIntent(scale: 1, lastScale: 1), isFalse);
    expect(isPanZoomScaleIntent(scale: 1.001, lastScale: 1), isFalse);
    expect(isPanZoomScaleIntent(scale: 1.01, lastScale: 1), isTrue);
    expect(isPanZoomScaleIntent(scale: 1.0, lastScale: 0.99), isTrue);
  });
}
