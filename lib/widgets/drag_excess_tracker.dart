/// Tracks drag movement with boundary overshoot, matching splitter behavior.
///
/// When a drag hits a boundary, the overshoot is stored and must be consumed by
/// dragging back before the effective value moves again.
class DragExcessTracker {
  double _excess = 0.0;
  double _effectiveValue = 0.0;

  void start(double value) {
    _excess = 0.0;
    _effectiveValue = value;
  }

  void sync(double value) {
    _effectiveValue = value;
  }

  double update({
    required double delta,
    required double min,
    required double max,
  }) {
    final desired = _effectiveValue + _excess + delta;
    final clamped = desired.clamp(min, max).toDouble();
    _excess = desired - clamped;
    _effectiveValue = clamped;
    return clamped;
  }
}
