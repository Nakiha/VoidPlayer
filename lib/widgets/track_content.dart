import 'package:flutter/material.dart';

/// Clip visualization area inside a TrackRow. CustomPaint with clip rect + playhead.
class TrackContent extends StatelessWidget {
  final double playheadPosition; // 0.0 - 1.0
  final Color clipColor;

  const TrackContent({
    super.key,
    this.playheadPosition = 0.0,
    this.clipColor = Colors.blueGrey,
  });

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      size: Size.infinite,
      painter: _TrackContentPainter(
        playheadPosition: playheadPosition,
        clipColor: clipColor,
        playheadColor: Theme.of(context).colorScheme.primary,
        bgColor: Theme.of(context).colorScheme.surfaceContainerLow,
      ),
    );
  }
}

class _TrackContentPainter extends CustomPainter {
  final double playheadPosition;
  final Color clipColor;
  final Color playheadColor;
  final Color bgColor;

  _TrackContentPainter({
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

    // Clip rectangle (90% width, centered vertically with padding)
    const clipWidthRatio = 0.9;
    final clipWidth = size.width * clipWidthRatio;
    final clipHeight = size.height - 8;
    final clipRect = Rect.fromLTWH(
      (size.width - clipWidth) / 2,
      4,
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
    final playheadX = clipRect.left + clipRect.width * playheadPosition.clamp(0.0, 1.0);
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
    return oldDelegate.playheadPosition != playheadPosition;
  }
}
