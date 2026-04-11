import 'package:flutter/material.dart';

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

  const TimelineSlider({
    super.key,
    required this.currentUs,
    required this.durationUs,
    required this.onSeek,
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

  @override
  Widget build(BuildContext context) {
    final accentColor = Theme.of(context).colorScheme.primary;
    final inactiveColor =
        Theme.of(context).colorScheme.surfaceContainerHighest;

    return Listener(
      behavior: HitTestBehavior.opaque,
      onPointerDown: (e) => _handleSeek(e.localPosition.dx),
      onPointerMove: (e) => _handleSeek(e.localPosition.dx),
      onPointerUp: (e) => _handleSeek(e.localPosition.dx),
      onPointerCancel: (_) {},
      onPointerHover: (event) {
        setState(() => _hoverX = event.localPosition.dx);
      },
      child: MouseRegion(
        opaque: false,
        onEnter: (event) => setState(() {
          _hovering = true;
          _hoverX = event.localPosition.dx;
        }),
        onExit: (_) => setState(() => _hovering = false),
        cursor: SystemMouseCursors.click,
        child: Stack(
          clipBehavior: Clip.none,
          children: [
            // Track
            SizedBox.expand(
              child: CustomPaint(
                painter: _TrackPainter(
                  value: widget.durationUs > 0
                      ? (widget.currentUs / widget.durationUs).clamp(0.0, 1.0)
                      : 0.0,
                  trackHeight: _trackHeight,
                  trackRadius: _trackRadius,
                  accentColor: accentColor,
                  inactiveColor: inactiveColor,
                ),
              ),
            ),
            // Tooltip (positioned above the track)
            if (_hovering && widget.durationUs > 0)
              Positioned(
                top: -(_tooltipHeight + _triangleSize + _tooltipOffset),
                left: 0,
                right: 0,
                child: IgnorePointer(
                  child: LayoutBuilder(
                    builder: (context, constraints) {
                      final width = constraints.maxWidth;
                      final clampedX = _hoverX.clamp(0.0, width);
                      return CustomPaint(
                        size: Size(width, _tooltipHeight + _triangleSize),
                        painter: _TooltipPainter(
                          hoverX: clampedX,
                          text: _formatTime(_xToUs(clampedX, width)),
                          accentColor: accentColor,
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

  void _handleSeek(double localX) {
    final box = context.findRenderObject() as RenderBox;
    final x = localX.clamp(0.0, box.size.width);
    setState(() => _hoverX = x);
    widget.onSeek(_xToUs(x, box.size.width));
  }

  int _xToUs(double x, double width) {
    if (width <= 0 || widget.durationUs <= 0) return 0;
    return ((x / width).clamp(0.0, 1.0) * widget.durationUs).round();
  }

  /// Format microseconds to MM:SS.ss (centiseconds, matching PySide6).
  static String _formatTime(int us) {
    final totalSeconds = us / 1000000.0;
    final minutes = totalSeconds.toInt() ~/ 60;
    final seconds = totalSeconds - minutes * 60;
    return '${minutes.toString().padLeft(2, '0')}:'
        '${seconds.toStringAsFixed(2).padLeft(5, '0')}';
  }
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

/// Paints the hover tooltip: accent-color rounded rect + downward triangle + time text.
class _TooltipPainter extends CustomPainter {
  final double hoverX;
  final String text;
  final Color accentColor;
  final double tooltipHeight;
  final double tooltipPadding;
  final double tooltipRadius;
  final double triangleSize;

  _TooltipPainter({
    required this.hoverX,
    required this.text,
    required this.accentColor,
    required this.tooltipHeight,
    required this.tooltipPadding,
    required this.tooltipRadius,
    required this.triangleSize,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (text.isEmpty) return;

    // Measure text
    final textColor = _getTextColor(accentColor);
    final textSpan = TextSpan(
      text: text,
      style: TextStyle(color: textColor, fontSize: 12),
    );
    final textPainter = TextPainter(
      text: textSpan,
      textDirection: TextDirection.ltr,
    )..layout();

    final textWidth = textPainter.width;
    final tooltipWidth = textWidth + tooltipPadding * 2;

    // Tooltip X: centered on hoverX, clamped to bounds
    var tooltipX = hoverX - tooltipWidth / 2;
    tooltipX = tooltipX.clamp(0.0, size.width - tooltipWidth);

    // Background (rounded rect)
    canvas.drawRRect(
      RRect.fromRectAndRadius(
        Rect.fromLTWH(tooltipX, 0, tooltipWidth, tooltipHeight),
        Radius.circular(tooltipRadius),
      ),
      Paint()..color = accentColor,
    );

    // Downward triangle pointing to hoverX
    final half = triangleSize / 2;
    canvas.drawPath(
      Path()
        ..moveTo(hoverX, tooltipHeight + triangleSize) // bottom point
        ..lineTo(hoverX - half, tooltipHeight) // top-left
        ..lineTo(hoverX + half, tooltipHeight) // top-right
        ..close(),
      Paint()..color = accentColor,
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

  static Color _getTextColor(Color bg) {
    return bg.computeLuminance() > 0.5
        ? const Color(0xFF000000)
        : const Color(0xFFFFFFFF);
  }

  @override
  bool shouldRepaint(covariant _TooltipPainter old) =>
      old.hoverX != hoverX ||
      old.text != text ||
      old.accentColor != accentColor;
}
