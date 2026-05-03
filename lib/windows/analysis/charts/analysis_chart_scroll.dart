import 'dart:math' as math;

import 'package:flutter/services.dart';

export '../../../utils/pointer_gesture_utils.dart' show isPanZoomScaleIntent;

const double _horizontalScrollDominanceRatio = 0.75;
const double _scaleZoomLogStep = 0.04;

bool isChartHorizontalScrollIntent(Offset scrollDelta) {
  final dx = scrollDelta.dx.abs();
  final dy = scrollDelta.dy.abs();
  return dx > 0 && dx >= dy * _horizontalScrollDominanceRatio;
}

bool isChartZoomModifierPressed() => HardwareKeyboard.instance.isControlPressed;

double chartZoomScrollDeltaForModifier(Offset scrollDelta) {
  if (scrollDelta.dy != 0) return scrollDelta.dy;
  return scrollDelta.dx;
}

bool handleChartHorizontalScrollPan({
  required Offset scrollDelta,
  required double chartExtent,
  required double viewStart,
  required double viewEnd,
  required double total,
  required ValueChanged<double> onPan,
}) {
  if (!isChartHorizontalScrollIntent(scrollDelta)) return false;
  if (total <= 0) return true;
  final span = (viewEnd - viewStart).clamp(1.0, total);
  final maxOffset = (total - span).clamp(0.0, double.infinity);
  if (maxOffset <= 0 || chartExtent <= 0) return true;

  final deltaFrames = -scrollDelta.dx / chartExtent * span;
  if (deltaFrames == 0) return true;
  onPan((viewStart + deltaFrames).clamp(0.0, maxOffset));
  return true;
}

int stableFrameTickStep({
  required double visibleSpan,
  required double plotExtent,
  double minTickGap = 92.0,
}) {
  if (visibleSpan <= 1 || plotExtent <= 0) return 1;
  final targetTickCount = (plotExtent / minTickGap).floor().clamp(
    1,
    double.infinity,
  );
  return (visibleSpan / targetTickCount).ceil().clamp(1, 1 << 30).toInt();
}

int firstStableFrameTick(double viewStart, int step) {
  if (step <= 1) return viewStart.floor();
  return (viewStart.floor() ~/ step) * step;
}

({double accumulator, double? scrollDelta}) accumulateScaleZoomScrollDelta({
  required double scaleDelta,
  required double accumulator,
}) {
  if (scaleDelta <= 0 || !scaleDelta.isFinite) {
    return (accumulator: accumulator, scrollDelta: null);
  }
  final next = accumulator + math.log(scaleDelta);
  if (next.abs() < _scaleZoomLogStep) {
    return (accumulator: next, scrollDelta: null);
  }
  return (accumulator: 0.0, scrollDelta: next > 0 ? -1.0 : 1.0);
}
