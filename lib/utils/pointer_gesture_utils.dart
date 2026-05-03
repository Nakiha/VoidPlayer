import 'dart:math' as math;

const double _panZoomScaleIntentLogThreshold = 0.004;

bool isPanZoomScaleIntent({required double scale, required double lastScale}) {
  if (scale <= 0 || lastScale <= 0 || !scale.isFinite || !lastScale.isFinite) {
    return false;
  }
  return math.log(scale).abs() >= _panZoomScaleIntentLogThreshold ||
      math.log(scale / lastScale).abs() >= _panZoomScaleIntentLogThreshold;
}
