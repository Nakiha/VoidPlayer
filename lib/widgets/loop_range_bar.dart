import 'package:flutter/material.dart';

import '../l10n/app_localizations.dart';
import 'drag_excess_tracker.dart';

enum LoopRangeHandle { start, end }

/// Loop range editor aligned to the shared timeline content column.
class LoopRangeBar extends StatelessWidget {
  final double timelineStartWidth;
  final bool enabled;
  final int startUs;
  final int endUs;
  final int durationUs;
  final ValueChanged<bool> onEnabledChanged;
  final void Function(int startUs, int endUs) onRangeChanged;
  final ValueChanged<LoopRangeHandle>? onRangeChangeEnd;

  const LoopRangeBar({
    super.key,
    required this.timelineStartWidth,
    required this.enabled,
    required this.startUs,
    required this.endUs,
    required this.durationUs,
    required this.onEnabledChanged,
    required this.onRangeChanged,
    this.onRangeChangeEnd,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    final l = AppLocalizations.of(context)!;
    return Container(
      height: 40,
      decoration: BoxDecoration(
        color: colorScheme.surfaceContainerLow,
        border: Border(
          top: BorderSide(color: colorScheme.outlineVariant),
          bottom: BorderSide(color: colorScheme.outlineVariant),
        ),
      ),
      child: Row(
        children: [
          SizedBox(
            width: timelineStartWidth,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 8),
              child: Opacity(
                opacity: durationUs > 0 ? 1.0 : 0.5,
                child: Row(
                  children: [
                    Text(
                      l.loopRange,
                      style: Theme.of(context).textTheme.bodySmall,
                      overflow: TextOverflow.ellipsis,
                    ),
                    const SizedBox(width: 8),
                    _CompactSwitch(
                      value: enabled,
                      onChanged: durationUs > 0 ? onEnabledChanged : null,
                    ),
                    const Spacer(),
                  ],
                ),
              ),
            ),
          ),
          Expanded(
            child: _LoopRangeTimeline(
              enabled: enabled,
              startUs: startUs,
              endUs: endUs,
              durationUs: durationUs,
              onRangeChanged: onRangeChanged,
              onRangeChangeEnd: onRangeChangeEnd,
            ),
          ),
        ],
      ),
    );
  }
}

class _CompactSwitch extends StatelessWidget {
  final bool value;
  final ValueChanged<bool>? onChanged;

  const _CompactSwitch({required this.value, required this.onChanged});

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    final enabled = onChanged != null;
    final trackColor = value
        ? colorScheme.primary
        : colorScheme.surfaceContainerHighest;
    final knobColor = value ? colorScheme.onPrimary : colorScheme.outline;

    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: enabled ? () => onChanged?.call(!value) : null,
      child: MouseRegion(
        cursor: enabled ? SystemMouseCursors.click : SystemMouseCursors.basic,
        child: SizedBox(
          width: 34,
          height: 22,
          child: Align(
            alignment: Alignment.center,
            child: AnimatedContainer(
              duration: const Duration(milliseconds: 140),
              width: 30,
              height: 16,
              padding: const EdgeInsets.all(2),
              decoration: BoxDecoration(
                color: trackColor,
                borderRadius: BorderRadius.circular(999),
                border: Border.all(
                  color: value ? colorScheme.primary : colorScheme.outline,
                  width: 1,
                ),
              ),
              child: AnimatedAlign(
                duration: const Duration(milliseconds: 140),
                curve: Curves.easeOut,
                alignment: value ? Alignment.centerRight : Alignment.centerLeft,
                child: Container(
                  width: 10,
                  height: 10,
                  decoration: BoxDecoration(
                    color: knobColor,
                    shape: BoxShape.circle,
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _LoopRangeTimeline extends StatefulWidget {
  static const double _margin = 8.0;
  static const double _trackHeight = 4.0;
  static const double _handleSize = 16.0;
  static const int _minRangeUs = 10000;

  final bool enabled;
  final int startUs;
  final int endUs;
  final int durationUs;
  final void Function(int startUs, int endUs) onRangeChanged;
  final ValueChanged<LoopRangeHandle>? onRangeChangeEnd;

  const _LoopRangeTimeline({
    required this.enabled,
    required this.startUs,
    required this.endUs,
    required this.durationUs,
    required this.onRangeChanged,
    this.onRangeChangeEnd,
  });

  @override
  State<_LoopRangeTimeline> createState() => _LoopRangeTimelineState();
}

class _LoopRangeTimelineState extends State<_LoopRangeTimeline> {
  final _startTracker = DragExcessTracker();
  final _endTracker = DragExcessTracker();

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    final range = _normalizedRange();

    return LayoutBuilder(
      builder: (context, constraints) {
        final width = constraints.maxWidth;
        final drawableWidth = (width - _LoopRangeTimeline._margin * 2).clamp(
          0.0,
          double.infinity,
        );
        final startX =
            _LoopRangeTimeline._margin + drawableWidth * _ratio(range.start);
        final endX =
            _LoopRangeTimeline._margin + drawableWidth * _ratio(range.end);

        return Stack(
          clipBehavior: Clip.none,
          children: [
            Positioned.fill(
              child: CustomPaint(
                painter: _LoopRangePainter(
                  enabled: widget.enabled,
                  startX: startX,
                  endX: endX,
                  margin: _LoopRangeTimeline._margin,
                  trackHeight: _LoopRangeTimeline._trackHeight,
                  activeColor: colorScheme.primary,
                  inactiveColor: colorScheme.surfaceContainerHighest,
                ),
              ),
            ),
            _buildHandle(
              context: context,
              left: startX - _LoopRangeTimeline._handleSize / 2,
              handle: LoopRangeHandle.start,
              currentX: startX,
              otherX: endX,
              drawableWidth: drawableWidth,
            ),
            _buildHandle(
              context: context,
              left: endX - _LoopRangeTimeline._handleSize / 2,
              handle: LoopRangeHandle.end,
              currentX: endX,
              otherX: startX,
              drawableWidth: drawableWidth,
            ),
          ],
        );
      },
    );
  }

  Widget _buildHandle({
    required BuildContext context,
    required double left,
    required LoopRangeHandle handle,
    required double currentX,
    required double otherX,
    required double drawableWidth,
  }) {
    final colorScheme = Theme.of(context).colorScheme;
    final interactive = widget.enabled && widget.durationUs > 0;
    return Positioned(
      left: left,
      top: (40 - _LoopRangeTimeline._handleSize) / 2,
      width: _LoopRangeTimeline._handleSize,
      height: _LoopRangeTimeline._handleSize,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onHorizontalDragStart: interactive
            ? (_) => _trackerFor(handle).start(currentX)
            : null,
        onHorizontalDragUpdate: interactive
            ? (details) => _dragHandle(
                handle: handle,
                dx: details.delta.dx,
                otherX: otherX,
                drawableWidth: drawableWidth,
              )
            : null,
        onHorizontalDragEnd: interactive
            ? (_) => widget.onRangeChangeEnd?.call(handle)
            : null,
        onHorizontalDragCancel: interactive
            ? () => widget.onRangeChangeEnd?.call(handle)
            : null,
        child: MouseRegion(
          cursor: interactive
              ? SystemMouseCursors.resizeLeftRight
              : SystemMouseCursors.basic,
          child: Center(
            child: Container(
              width: 11,
              height: 14,
              decoration: BoxDecoration(
                color: widget.enabled
                    ? colorScheme.primary
                    : colorScheme.onSurfaceVariant,
                border: Border.all(color: colorScheme.surface, width: 1.5),
                borderRadius: BorderRadius.circular(2),
              ),
            ),
          ),
        ),
      ),
    );
  }

  void _dragHandle({
    required LoopRangeHandle handle,
    required double dx,
    required double otherX,
    required double drawableWidth,
  }) {
    if (!widget.enabled || widget.durationUs <= 0 || drawableWidth <= 0) {
      return;
    }

    final minRangeUs = widget.durationUs > _LoopRangeTimeline._minRangeUs
        ? _LoopRangeTimeline._minRangeUs
        : 0;
    final minRangePx = minRangeUs / widget.durationUs * drawableWidth;
    final range = _normalizedRange();
    var nextStart = range.start;
    var nextEnd = range.end;
    final tracker = _trackerFor(handle);

    switch (handle) {
      case LoopRangeHandle.start:
        final nextX = tracker.update(
          delta: dx,
          min: _LoopRangeTimeline._margin,
          max: otherX - minRangePx,
        );
        nextStart = _xToUs(nextX, drawableWidth).clamp(0, nextEnd).toInt();
      case LoopRangeHandle.end:
        final nextX = tracker.update(
          delta: dx,
          min: otherX + minRangePx,
          max: _LoopRangeTimeline._margin + drawableWidth,
        );
        nextEnd = _xToUs(
          nextX,
          drawableWidth,
        ).clamp(nextStart, widget.durationUs).toInt();
    }

    widget.onRangeChanged(nextStart, nextEnd);
  }

  ({int start, int end}) _normalizedRange() {
    if (widget.durationUs <= 0) return (start: 0, end: 0);
    final minRangeUs = widget.durationUs > _LoopRangeTimeline._minRangeUs
        ? _LoopRangeTimeline._minRangeUs
        : 0;
    final start = widget.startUs.clamp(0, widget.durationUs).toInt();
    final end = widget.endUs
        .clamp(start + minRangeUs, widget.durationUs)
        .toInt();
    return (start: start, end: end);
  }

  double _ratio(int us) {
    if (widget.durationUs <= 0) return 0.0;
    return (us / widget.durationUs).clamp(0.0, 1.0);
  }

  int _xToUs(double x, double drawableWidth) {
    final localX = (x - _LoopRangeTimeline._margin).clamp(0.0, drawableWidth);
    return (localX / drawableWidth * widget.durationUs).round();
  }

  DragExcessTracker _trackerFor(LoopRangeHandle handle) {
    return switch (handle) {
      LoopRangeHandle.start => _startTracker,
      LoopRangeHandle.end => _endTracker,
    };
  }
}

class _LoopRangePainter extends CustomPainter {
  final bool enabled;
  final double startX;
  final double endX;
  final double margin;
  final double trackHeight;
  final Color activeColor;
  final Color inactiveColor;

  _LoopRangePainter({
    required this.enabled,
    required this.startX,
    required this.endX,
    required this.margin,
    required this.trackHeight,
    required this.activeColor,
    required this.inactiveColor,
  });

  @override
  void paint(Canvas canvas, Size size) {
    final y = (size.height - trackHeight) / 2;
    final trackRect = Rect.fromLTWH(
      margin,
      y,
      (size.width - margin * 2).clamp(0.0, double.infinity),
      trackHeight,
    );
    canvas.drawRect(trackRect, Paint()..color = inactiveColor);

    if (endX > startX) {
      canvas.drawRect(
        Rect.fromLTRB(startX, y, endX, y + trackHeight),
        Paint()..color = activeColor.withValues(alpha: enabled ? 0.42 : 0.16),
      );
    }
  }

  @override
  bool shouldRepaint(covariant _LoopRangePainter oldDelegate) {
    return oldDelegate.enabled != enabled ||
        oldDelegate.startX != startX ||
        oldDelegate.endX != endX ||
        oldDelegate.activeColor != activeColor ||
        oldDelegate.inactiveColor != inactiveColor;
  }
}
