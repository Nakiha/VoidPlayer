import 'package:flutter/material.dart';

/// Clip visualization area inside a TrackRow.
///
/// The clip rectangle width is proportional to [durationRatio] (0.0–1.0)
/// relative to the longest track. [playheadPosition] is already per-track
/// clamped (0.0–1.0 within this clip's duration).
class TrackContent extends StatelessWidget {
  /// Ratio of this track's duration to the longest track (0.0–1.0).
  final double durationRatio;

  /// Per-track clamped playhead position (0.0–1.0 within this clip).
  final double playheadPosition;

  final Color clipColor;

  const TrackContent({
    super.key,
    this.durationRatio = 1.0,
    this.playheadPosition = 0.0,
    this.clipColor = Colors.blueGrey,
  });

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      size: Size.infinite,
      painter: _TrackContentPainter(
        durationRatio: durationRatio.clamp(0.0, 1.0),
        playheadPosition: playheadPosition.clamp(0.0, 1.0),
        clipColor: clipColor,
        playheadColor: Theme.of(context).colorScheme.primary,
        bgColor: Theme.of(context).colorScheme.surfaceContainerLow,
      ),
    );
  }
}

class _TrackContentPainter extends CustomPainter {
  final double durationRatio;
  final double playheadPosition;
  final Color clipColor;
  final Color playheadColor;
  final Color bgColor;

  _TrackContentPainter({
    required this.durationRatio,
    required this.playheadPosition,
    required this.clipColor,
    required this.playheadColor,
    required this.bgColor,
  });

  @override
  void paint(Canvas canvas, Size size) {
    // Background
    canvas.drawRect(
      Rect.fromLTWH(0, 0, size.width, size.height),
      Paint()..color = bgColor,
    );

    // Clip rectangle: width proportional to duration, 8px margin all sides
    const margin = 8.0;
    final clipWidth = (size.width - margin * 2) * durationRatio;
    final clipHeight = size.height - margin * 2;
    if (clipWidth <= 0 || clipHeight <= 0) return;
    final clipRect = Rect.fromLTWH(
      margin,
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
  }

  @override
  bool shouldRepaint(covariant _TrackContentPainter oldDelegate) {
    return oldDelegate.playheadPosition != playheadPosition ||
        oldDelegate.durationRatio != durationRatio;
  }
}
