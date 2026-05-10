import 'package:flutter/material.dart';
import '../utils/pts_range.dart';
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
  final int trackStartTimeUs;
  final int offsetUs;
  final int maxEffectiveDurationUs;
  final List<int> markerPtsUs;
  final bool loopRangeEnabled;
  final int loopStartUs;
  final int loopEndUs;

  const TrackContent({
    super.key,
    this.durationRatio = 1.0,
    this.offsetRatio = 0.0,
    this.playheadPosition = 0.0,
    this.clipColor = Colors.blueGrey,
    this.hoverPtsUs = 0,
    this.sliderHovering = false,
    this.trackDurationUs = 0,
    this.trackStartTimeUs = 0,
    this.offsetUs = 0,
    this.maxEffectiveDurationUs = 0,
    this.markerPtsUs = const [],
    this.loopRangeEnabled = false,
    this.loopStartUs = 0,
    this.loopEndUs = 0,
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
        trackStartTimeUs: trackStartTimeUs,
        offsetUs: offsetUs,
        maxEffectiveDurationUs: maxEffectiveDurationUs,
        markerPtsUs: markerPtsUs,
        loopRangeEnabled: loopRangeEnabled,
        loopStartUs: loopStartUs,
        loopEndUs: loopEndUs,
        rangeColor: Theme.of(context).colorScheme.primary,
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
  final int trackStartTimeUs;
  final int offsetUs;
  final int maxEffectiveDurationUs;
  final List<int> markerPtsUs;
  final bool loopRangeEnabled;
  final int loopStartUs;
  final int loopEndUs;
  final Color rangeColor;

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
    required this.trackStartTimeUs,
    required this.offsetUs,
    required this.maxEffectiveDurationUs,
    required this.markerPtsUs,
    required this.loopRangeEnabled,
    required this.loopStartUs,
    required this.loopEndUs,
    required this.rangeColor,
  });

  @override
  void paint(Canvas canvas, Size size) {
    const horizontalMargin = 4.0;
    const verticalMargin = 4.0;
    const borderWidth = 1.0;
    final drawableWidth = size.width - horizontalMargin * 2;

    // Background
    canvas.drawRect(
      Rect.fromLTWH(0, 0, size.width, size.height),
      Paint()..color = bgColor,
    );

    // Clip rectangle: starts at offset position, width proportional to duration
    final clipX = horizontalMargin + drawableWidth * offsetRatio;
    final clipWidth = drawableWidth * durationRatio;
    final clipHeight = size.height - verticalMargin * 2;
    if (clipWidth <= 0 || clipHeight <= 0) return;
    final clipRect = Rect.fromLTWH(
      clipX,
      verticalMargin,
      clipWidth,
      clipHeight,
    );
    canvas.drawRect(
      clipRect,
      Paint()..color = clipColor.withValues(alpha: 0.3),
    );

    if (loopRangeEnabled && trackDurationUs > 0 && loopEndUs > loopStartUs) {
      final trackStartUs = trackStartTimeUs + offsetUs;
      final trackEndUs =
          trackPtsEndUs(
            startTimeUs: trackStartTimeUs,
            durationUs: trackDurationUs,
          ) +
          offsetUs;
      final selectedStartUs = loopStartUs
          .clamp(trackStartUs, trackEndUs)
          .toInt();
      final selectedEndUs = loopEndUs.clamp(trackStartUs, trackEndUs).toInt();
      if (selectedEndUs > selectedStartUs) {
        final playableDurationUs = trackEndUs - trackStartUs;
        final startRatio =
            (selectedStartUs - trackStartUs) / playableDurationUs;
        final endRatio = (selectedEndUs - trackStartUs) / playableDurationUs;
        final highlightRect = Rect.fromLTRB(
          clipRect.left + clipRect.width * startRatio,
          clipRect.top,
          clipRect.left + clipRect.width * endRatio,
          clipRect.bottom,
        );
        canvas.drawRect(
          highlightRect,
          Paint()..color = rangeColor.withValues(alpha: 0.16),
        );
      }
    }

    canvas.drawRect(
      clipRect.deflate(borderWidth / 2),
      Paint()
        ..color = clipColor.withValues(alpha: 0.6)
        ..style = PaintingStyle.stroke
        ..strokeWidth = borderWidth,
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

    final markers = <int>[...markerPtsUs, if (sliderHovering) hoverPtsUs];
    for (final markerUs in markers) {
      _drawTimeMarker(
        canvas: canvas,
        size: size,
        clipRect: clipRect,
        horizontalMargin: horizontalMargin,
        verticalMargin: verticalMargin,
        drawableWidth: drawableWidth,
        markerPtsUs: markerUs,
      );
    }
  }

  void _drawTimeMarker({
    required Canvas canvas,
    required Size size,
    required Rect clipRect,
    required double horizontalMargin,
    required double verticalMargin,
    required double drawableWidth,
    required int markerPtsUs,
  }) {
    if (maxEffectiveDurationUs <= 0) return;

    final markerGlobalRatio = (markerPtsUs / maxEffectiveDurationUs).clamp(
      0.0,
      1.0,
    );
    var markerX = horizontalMargin + drawableWidth * markerGlobalRatio;
    // Clamp marker line within the clip rect bounds
    markerX = markerX.clamp(clipRect.left, clipRect.right);

    // Draw dashed vertical line
    final dashPaint = Paint()
      ..color = playheadColor.withValues(alpha: 0.5)
      ..strokeWidth = 1.0;

    double y = 0;
    while (y < size.height) {
      final endY = (y + 4.0).clamp(0.0, size.height);
      canvas.drawLine(Offset(markerX, y), Offset(markerX, endY), dashPaint);
      y += 7.0;
    }

    // Source PTS label — translate global timeline time back through the
    // track offset, then clamp to this track's real source PTS range.
    final sourcePtsUs = (markerPtsUs - offsetUs)
        .clamp(
          trackStartTimeUs,
          trackPtsEndUs(
            startTimeUs: trackStartTimeUs,
            durationUs: trackDurationUs,
          ),
        )
        .toInt();
    final label = formatTimeShort(sourcePtsUs);
    final textSpan = TextSpan(
      text: label,
      style: TextStyle(
        color: playheadColor.withValues(alpha: 0.8),
        fontSize: 9,
      ),
    );
    final tp = TextPainter(text: textSpan, textDirection: TextDirection.ltr)
      ..layout();
    // Position label to the right of the dashed line, clamped to bounds
    final labelX = (markerX + 4).clamp(
      clipRect.left,
      size.width - tp.width - 4,
    );
    tp.paint(canvas, Offset(labelX, verticalMargin));
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
        oldDelegate.trackStartTimeUs != trackStartTimeUs ||
        oldDelegate.offsetUs != offsetUs ||
        oldDelegate.maxEffectiveDurationUs != maxEffectiveDurationUs ||
        oldDelegate.markerPtsUs != markerPtsUs ||
        oldDelegate.loopRangeEnabled != loopRangeEnabled ||
        oldDelegate.loopStartUs != loopStartUs ||
        oldDelegate.loopEndUs != loopEndUs ||
        oldDelegate.rangeColor != rangeColor;
  }
}
