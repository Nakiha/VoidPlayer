import 'package:flutter/material.dart';
import '../utils/time_format.dart';

/// Clip visualization area inside a TrackRow.
///
/// The clip rectangle width is proportional to [durationRatio] (0.0–1.0)
/// relative to the max effective duration. [offsetRatio] shifts the clip
/// start position. [playheadPosition] is per-track clamped (0.0–1.0 within
/// this clip's duration).
class TrackContent extends StatelessWidget {
  /// Ratio of this track's original duration to the max effective duration (0.0–1.0).
  final double durationRatio;

  /// Ratio of this track's offset to the max effective duration (clip start position).
  final double offsetRatio;

  /// Per-track clamped playhead position (0.0–1.0 within this clip).
  final double playheadPosition;

  final Color clipColor;

  // Hover cross-track indicator state
  final int hoverPtsUs;
  final bool sliderHovering;
  final int trackDurationUs;
  final int offsetUs;
  final int maxEffectiveDurationUs;

  const TrackContent({
    super.key,
    this.durationRatio = 1.0,
    this.offsetRatio = 0.0,
    this.playheadPosition = 0.0,
    this.clipColor = Colors.blueGrey,
    this.hoverPtsUs = 0,
    this.sliderHovering = false,
    this.trackDurationUs = 0,
    this.offsetUs = 0,
    this.maxEffectiveDurationUs = 0,
  });

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      size: Size.infinite,
      painter: _TrackContentPainter(
        durationRatio: durationRatio.clamp(0.0, 1.0),
        offsetRatio: offsetRatio.clamp(0.0, 1.0),
        playheadPosition: playheadPosition.clamp(0.0, 1.0),
        clipColor: clipColor,
        playheadColor: Theme.of(context).colorScheme.primary,
        bgColor: Theme.of(context).colorScheme.surfaceContainerLow,
        hoverPtsUs: hoverPtsUs,
        sliderHovering: sliderHovering,
        trackDurationUs: trackDurationUs,
        offsetUs: offsetUs,
        maxEffectiveDurationUs: maxEffectiveDurationUs,
      ),
    );
  }
}

class _TrackContentPainter extends CustomPainter {
  final double durationRatio;
  final double offsetRatio;
  final double playheadPosition;
  final Color clipColor;
  final Color playheadColor;
  final Color bgColor;

  // Hover state
  final int hoverPtsUs;
  final bool sliderHovering;
  final int trackDurationUs;
  final int offsetUs;
  final int maxEffectiveDurationUs;

  _TrackContentPainter({
    required this.durationRatio,
    required this.offsetRatio,
    required this.playheadPosition,
    required this.clipColor,
    required this.playheadColor,
    required this.bgColor,
    required this.hoverPtsUs,
    required this.sliderHovering,
    required this.trackDurationUs,
    required this.offsetUs,
    required this.maxEffectiveDurationUs,
  });

  @override
  void paint(Canvas canvas, Size size) {
    const margin = 8.0;
    final drawableWidth = size.width - margin * 2;

    // Background
    canvas.drawRect(
      Rect.fromLTWH(0, 0, size.width, size.height),
      Paint()..color = bgColor,
    );

    // Clip rectangle: starts at offset position, width proportional to duration
    final clipX = margin + drawableWidth * offsetRatio;
    final clipWidth = drawableWidth * durationRatio;
    final clipHeight = size.height - margin * 2;
    if (clipWidth <= 0 || clipHeight <= 0) return;
    final clipRect = Rect.fromLTWH(
      clipX,
      margin,
      clipWidth,
      clipHeight,
    );
    canvas.drawRect(clipRect, Paint()..color = clipColor.withValues(alpha: 0.3));
    canvas.drawRect(
      clipRect,
      Paint()
        ..color = clipColor.withValues(alpha: 0.6)
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1,
    );

    // Playhead line
    final playheadX = clipRect.left + clipRect.width * playheadPosition;
    canvas.drawLine(
      Offset(playheadX, 0),
      Offset(playheadX, size.height),
      Paint()
        ..color = playheadColor
        ..strokeWidth = 1.5,
    );

    // Hover dashed line + local time label
    if (sliderHovering && maxEffectiveDurationUs > 0) {
      final hoverGlobalRatio = (hoverPtsUs / maxEffectiveDurationUs).clamp(0.0, 1.0);
      var hoverX = margin + drawableWidth * hoverGlobalRatio;
      // Clamp hover line within the clip rect bounds
      hoverX = hoverX.clamp(clipRect.left, clipRect.right);

      // Draw dashed vertical line
      final dashPaint = Paint()
        ..color = playheadColor.withValues(alpha: 0.5)
        ..strokeWidth = 1.0;

      double y = 0;
      while (y < size.height) {
        final endY = (y + 4.0).clamp(0.0, size.height);
        canvas.drawLine(Offset(hoverX, y), Offset(hoverX, endY), dashPaint);
        y += 7.0;
      }

      // Local time label — clamp local time to track bounds for display
      final localTimeUs = (hoverPtsUs - offsetUs).clamp(0, trackDurationUs);
      final label = formatTimeShort(localTimeUs);
      final textSpan = TextSpan(
        text: label,
        style: TextStyle(color: playheadColor.withValues(alpha: 0.8), fontSize: 9),
      );
      final tp = TextPainter(
        text: textSpan,
        textDirection: TextDirection.ltr,
      )..layout();
      // Position label to the right of the dashed line, clamped to bounds
      final labelX = (hoverX + 4).clamp(clipX, size.width - tp.width - 4);
      tp.paint(canvas, Offset(labelX, margin));
    }
  }

  @override
  bool shouldRepaint(covariant _TrackContentPainter oldDelegate) {
    return oldDelegate.playheadPosition != playheadPosition ||
        oldDelegate.durationRatio != durationRatio ||
        oldDelegate.offsetRatio != offsetRatio ||
        oldDelegate.sliderHovering != sliderHovering ||
        oldDelegate.hoverPtsUs != hoverPtsUs ||
        oldDelegate.clipColor != clipColor ||
        oldDelegate.bgColor != bgColor ||
        oldDelegate.playheadColor != playheadColor ||
        oldDelegate.trackDurationUs != trackDurationUs ||
        oldDelegate.offsetUs != offsetUs ||
        oldDelegate.maxEffectiveDurationUs != maxEffectiveDurationUs;
  }
}
