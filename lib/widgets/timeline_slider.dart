import 'package:flutter/material.dart';
import '../utils/time_format.dart';

/// TimelineSlider with hover tooltip, matching PySide6 TimelineSlider behavior.
///
/// Features:
/// - 6px track with rounded ends, progress fill in accent color
/// - Hover tooltip showing time (MM:SS.ss) at cursor position
/// - Tooltip: accent-color rounded rect + downward triangle + contrasting text
/// - Click/drag to seek
class TimelineSlider extends StatefulWidget {
  final int currentUs;
  final int durationUs;
  final ValueChanged<int> onSeek;
  final void Function(int hoverUs, bool hovering)? onHoverChanged;
  final List<int> markerUs;
  final int? seekMinUs;
  final int? seekMaxUs;

  const TimelineSlider({
    super.key,
    required this.currentUs,
    required this.durationUs,
    required this.onSeek,
    this.onHoverChanged,
    this.markerUs = const [],
    this.seekMinUs,
    this.seekMaxUs,
  });

  @override
  State<TimelineSlider> createState() => _TimelineSliderState();
}

class _TimelineSliderState extends State<TimelineSlider> {
  static const double _trackHeight = 6.0;
  static const double _trackRadius = 3.0;
  static const double _tooltipHeight = 22.0;
  static const double _tooltipPadding = 8.0;
  static const double _tooltipRadius = 4.0;
  static const double _triangleSize = 6.0;
  static const double _tooltipOffset = 4.0;

  bool _hovering = false;
  double _hoverX = 0.0;
  bool _dragging = false;
  int? _dragPreviewUs;

  @override
  void dispose() {
    widget.onHoverChanged?.call(0, false);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final accentColor = Theme.of(context).colorScheme.primary;
    final inactiveColor = Theme.of(context).colorScheme.surfaceContainerHighest;
    final markerTooltipColor = Theme.of(
      context,
    ).colorScheme.surfaceContainerHighest;

    return Listener(
      behavior: HitTestBehavior.opaque,
      onPointerDown: (event) => _beginDrag(event.localPosition.dx),
      onPointerMove: (event) => _updateDrag(event.localPosition.dx),
      onPointerUp: (event) => _commitDrag(event.localPosition.dx),
      onPointerCancel: (_) => _cancelDrag(),
      onPointerHover: (event) {
        setState(() => _hoverX = event.localPosition.dx);
        _reportHover(event.localPosition.dx);
      },
      child: MouseRegion(
        opaque: false,
        onEnter: (event) {
          setState(() {
            _hovering = true;
            _hoverX = event.localPosition.dx;
          });
          _reportHover(event.localPosition.dx);
        },
        onExit: (_) {
          setState(() => _hovering = false);
          widget.onHoverChanged?.call(0, false);
        },
        cursor: SystemMouseCursors.click,
        child: Stack(
          clipBehavior: Clip.none,
          children: [
            // Track
            SizedBox.expand(
              child: CustomPaint(
                painter: _TrackPainter(
                  value: widget.durationUs > 0
                      ? (((_dragging ? _dragPreviewUs : widget.currentUs) ??
                                    0) /
                                widget.durationUs)
                            .clamp(0.0, 1.0)
                      : 0.0,
                  trackHeight: _trackHeight,
                  trackRadius: _trackRadius,
                  accentColor: accentColor,
                  inactiveColor: inactiveColor,
                ),
              ),
            ),
            // Fixed loop markers + hover tooltip (positioned above the track)
            if (((_hovering && widget.durationUs > 0) ||
                    widget.markerUs.isNotEmpty) &&
                widget.durationUs > 0)
              Positioned(
                top: -(_tooltipHeight + _triangleSize + _tooltipOffset),
                left: 0,
                right: 0,
                child: IgnorePointer(
                  child: LayoutBuilder(
                    builder: (context, constraints) {
                      final width = constraints.maxWidth;
                      final clampedX = _hoverX.clamp(0.0, width);
                      final tooltipSize = Size(
                        width,
                        _tooltipHeight + _triangleSize,
                      );
                      final markers = widget.markerUs
                          .where((us) => us >= 0 && us <= widget.durationUs)
                          .map(
                            (us) => _TooltipEntry(
                              x: _usToX(us, width),
                              text: _formatTime(us),
                              color: markerTooltipColor,
                            ),
                          )
                          .toList();
                      if (_hovering) {
                        markers.add(
                          _TooltipEntry(
                            x: clampedX,
                            text: _formatTime(_xToUs(clampedX, width)),
                            color: accentColor,
                          ),
                        );
                      }
                      return CustomPaint(
                        size: tooltipSize,
                        painter: _TooltipPainter(
                          entries: markers,
                          tooltipHeight: _tooltipHeight,
                          tooltipPadding: _tooltipPadding,
                          tooltipRadius: _tooltipRadius,
                          triangleSize: _triangleSize,
                        ),
                      );
                    },
                  ),
                ),
              ),
          ],
        ),
      ),
    );
  }

  void _beginDrag(double localX) {
    final previewUs = _updatePreview(localX);
    setState(() {
      _dragging = true;
      _dragPreviewUs = previewUs;
    });
  }

  void _updateDrag(double localX) {
    if (!_dragging) return;
    final previewUs = _updatePreview(localX);
    setState(() => _dragPreviewUs = previewUs);
  }

  void _commitDrag(double localX) {
    final targetUs = _updatePreview(localX);
    setState(() {
      _dragging = false;
      _dragPreviewUs = null;
    });
    widget.onSeek(targetUs);
  }

  void _cancelDrag() {
    if (!_dragging) return;
    setState(() {
      _dragging = false;
      _dragPreviewUs = null;
    });
  }

  int _updatePreview(double localX) {
    final box = context.findRenderObject() as RenderBox;
    final x = localX.clamp(0.0, box.size.width);
    _hoverX = x;
    final previewUs = _clampSeekUs(_xToUs(x, box.size.width));
    widget.onHoverChanged?.call(previewUs, true);
    return previewUs;
  }

  int _xToUs(double x, double width) {
    if (width <= 0 || widget.durationUs <= 0) return 0;
    return ((x / width).clamp(0.0, 1.0) * widget.durationUs).round();
  }

  double _usToX(int us, double width) {
    if (width <= 0 || widget.durationUs <= 0) return 0;
    return (us / widget.durationUs).clamp(0.0, 1.0) * width;
  }

  int _clampSeekUs(int us) {
    final minUs = widget.seekMinUs?.clamp(0, widget.durationUs).toInt() ?? 0;
    final maxUs =
        widget.seekMaxUs?.clamp(minUs, widget.durationUs).toInt() ??
        widget.durationUs;
    return us.clamp(minUs, maxUs).toInt();
  }

  void _reportHover(double localX) {
    final box = context.findRenderObject() as RenderBox;
    final x = localX.clamp(0.0, box.size.width);
    widget.onHoverChanged?.call(_xToUs(x, box.size.width), true);
  }

  /// Format microseconds to MM:SS.ss (centiseconds, matching PySide6).
  static String _formatTime(int us) => formatTimePad2(us);
}

/// Paints the slider track: inactive background + active progress fill.
class _TrackPainter extends CustomPainter {
  final double value;
  final double trackHeight;
  final double trackRadius;
  final Color accentColor;
  final Color inactiveColor;

  _TrackPainter({
    required this.value,
    required this.trackHeight,
    required this.trackRadius,
    required this.accentColor,
    required this.inactiveColor,
  });

  @override
  void paint(Canvas canvas, Size size) {
    final trackY = (size.height - trackHeight) / 2;
    final trackRect = RRect.fromRectAndRadius(
      Rect.fromLTWH(0, trackY, size.width, trackHeight),
      Radius.circular(trackRadius),
    );

    // Background
    canvas.drawRRect(trackRect, Paint()..color = inactiveColor);

    // Progress fill (clip to left portion, draw full track)
    if (value > 0) {
      canvas.save();
      canvas.clipRect(
        Rect.fromLTWH(0, trackY, value * size.width, trackHeight),
      );
      canvas.drawRRect(trackRect, Paint()..color = accentColor);
      canvas.restore();
    }
  }

  @override
  bool shouldRepaint(covariant _TrackPainter old) =>
      old.value != value ||
      old.accentColor != accentColor ||
      old.inactiveColor != inactiveColor;
}

class _TooltipEntry {
  final double x;
  final String text;
  final Color color;

  const _TooltipEntry({
    required this.x,
    required this.text,
    required this.color,
  });
}

/// Paints time tooltips used by hover and fixed range markers.
class _TooltipPainter extends CustomPainter {
  final List<_TooltipEntry> entries;
  final double tooltipHeight;
  final double tooltipPadding;
  final double tooltipRadius;
  final double triangleSize;

  _TooltipPainter({
    required this.entries,
    required this.tooltipHeight,
    required this.tooltipPadding,
    required this.tooltipRadius,
    required this.triangleSize,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (entries.isEmpty) return;

    for (final entry in entries) {
      if (entry.text.isEmpty) continue;

      // Measure text
      final textColor = _getTextColor(entry.color);
      final textSpan = TextSpan(
        text: entry.text,
        style: TextStyle(color: textColor, fontSize: 12),
      );
      final textPainter = TextPainter(
        text: textSpan,
        textDirection: TextDirection.ltr,
      )..layout();

      final textWidth = textPainter.width;
      final tooltipWidth = textWidth + tooltipPadding * 2;

      // Tooltip X: centered on marker, clamped to bounds
      var tooltipX = entry.x - tooltipWidth / 2;
      tooltipX = tooltipX.clamp(0.0, size.width - tooltipWidth);
      final tooltipRect = Rect.fromLTWH(
        tooltipX,
        0,
        tooltipWidth,
        tooltipHeight,
      );

      // Background (rounded rect)
      canvas.drawRRect(
        RRect.fromRectAndRadius(tooltipRect, Radius.circular(tooltipRadius)),
        Paint()..color = entry.color,
      );

      // Downward triangle. Near the viewport edges, keep the triangle inside
      // the rounded rect's safe interior so it is not half-clipped.
      final half = triangleSize / 2;
      final triangleX = entry.x
          .clamp(
            tooltipRect.left + tooltipRadius + half,
            tooltipRect.right - tooltipRadius - half,
          )
          .toDouble();
      canvas.drawPath(
        Path()
          ..moveTo(triangleX, tooltipHeight + triangleSize)
          ..lineTo(triangleX - half, tooltipHeight - 0.5)
          ..lineTo(triangleX + half, tooltipHeight - 0.5)
          ..close(),
        Paint()..color = entry.color,
      );

      // Text (centered in tooltip)
      textPainter.paint(
        canvas,
        Offset(
          tooltipX + (tooltipWidth - textWidth) / 2,
          (tooltipHeight - textPainter.height) / 2,
        ),
      );
    }
  }

  static Color _getTextColor(Color bg) {
    return bg.computeLuminance() > 0.5
        ? const Color(0xFF000000)
        : const Color(0xFFFFFFFF);
  }

  @override
  bool shouldRepaint(covariant _TooltipPainter old) => old.entries != entries;
}
