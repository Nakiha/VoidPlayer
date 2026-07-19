import 'package:flutter/material.dart';

import 'drag_excess_tracker.dart';

/// Vertical splitter with a narrow visual line and a wider drag hit area.
///
/// The value is updated by horizontal drag deltas and clamped to [minValue] /
/// [maxValue]. [deltaScale] can be -1 when the controlled panel grows while the
/// pointer moves left, as with a right-side sidebar.
class ResizableVerticalDivider extends StatefulWidget {
  final Color? color;
  final double value;
  final double minValue;
  final double maxValue;
  final double deltaScale;
  final ValueChanged<double> onValueChanged;

  const ResizableVerticalDivider({
    super.key,
    this.color,
    required this.value,
    required this.minValue,
    required this.maxValue,
    this.deltaScale = 1.0,
    required this.onValueChanged,
  });

  @override
  State<ResizableVerticalDivider> createState() =>
      _ResizableVerticalDividerState();
}

class _ResizableVerticalDividerState extends State<ResizableVerticalDivider> {
  var _hovering = false;
  final _dragTracker = DragExcessTracker();
  late double _effectiveValue;

  @override
  void initState() {
    super.initState();
    _effectiveValue = widget.value;
  }

  @override
  void didUpdateWidget(covariant ResizableVerticalDivider oldWidget) {
    super.didUpdateWidget(oldWidget);
    _effectiveValue = widget.value;
    _dragTracker.sync(_effectiveValue);
  }

  void _onDragStart(DragStartDetails details) {
    _effectiveValue = widget.value;
    _dragTracker.start(_effectiveValue);
  }

  void _onDragUpdate(DragUpdateDetails details) {
    _effectiveValue = _dragTracker.update(
      delta: details.delta.dx * widget.deltaScale,
      min: widget.minValue,
      max: widget.maxValue,
    );
    widget.onValueChanged(_effectiveValue);
  }

  @override
  Widget build(BuildContext context) {
    final color = widget.color ?? Theme.of(context).colorScheme.outlineVariant;
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onHorizontalDragStart: _onDragStart,
      onHorizontalDragUpdate: _onDragUpdate,
      child: MouseRegion(
        cursor: SystemMouseCursors.resizeColumn,
        onEnter: (_) => setState(() => _hovering = true),
        onExit: (_) => setState(() => _hovering = false),
        child: SizedBox.expand(
          child: Center(
            child: Container(
              width: _hovering ? 2 : 0,
              color: _hovering
                  ? Theme.of(context).colorScheme.primary
                  : color.withValues(alpha: 0),
            ),
          ),
        ),
      ),
    );
  }
}

/// Horizontal splitter with a wider drag hit area than its visual line.
///
/// [deltaScale] can be -1 when the controlled panel grows while the pointer
/// moves up, as with a bottom deck.
class ResizableHorizontalDivider extends StatefulWidget {
  final Color? color;
  final double value;
  final double minValue;
  final double maxValue;
  final double deltaScale;
  final ValueChanged<double> onValueChanged;

  const ResizableHorizontalDivider({
    super.key,
    this.color,
    required this.value,
    required this.minValue,
    required this.maxValue,
    this.deltaScale = 1.0,
    required this.onValueChanged,
  });

  @override
  State<ResizableHorizontalDivider> createState() =>
      _ResizableHorizontalDividerState();
}

class _ResizableHorizontalDividerState
    extends State<ResizableHorizontalDivider> {
  var _hovering = false;
  final _dragTracker = DragExcessTracker();
  late double _effectiveValue;

  @override
  void initState() {
    super.initState();
    _effectiveValue = widget.value;
  }

  @override
  void didUpdateWidget(covariant ResizableHorizontalDivider oldWidget) {
    super.didUpdateWidget(oldWidget);
    _effectiveValue = widget.value;
    _dragTracker.sync(_effectiveValue);
  }

  void _onDragStart(DragStartDetails details) {
    _effectiveValue = widget.value;
    _dragTracker.start(_effectiveValue);
  }

  void _onDragUpdate(DragUpdateDetails details) {
    _effectiveValue = _dragTracker.update(
      delta: details.delta.dy * widget.deltaScale,
      min: widget.minValue,
      max: widget.maxValue,
    );
    widget.onValueChanged(_effectiveValue);
  }

  @override
  Widget build(BuildContext context) {
    final color = widget.color ?? Theme.of(context).colorScheme.outlineVariant;
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onVerticalDragStart: _onDragStart,
      onVerticalDragUpdate: _onDragUpdate,
      child: MouseRegion(
        cursor: SystemMouseCursors.resizeRow,
        onEnter: (_) => setState(() => _hovering = true),
        onExit: (_) => setState(() => _hovering = false),
        child: SizedBox.expand(
          child: Center(
            child: Container(
              height: _hovering ? 2 : 1,
              color: _hovering ? Theme.of(context).colorScheme.primary : color,
            ),
          ),
        ),
      ),
    );
  }
}
