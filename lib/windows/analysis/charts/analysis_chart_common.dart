import 'package:flutter/material.dart';

import '../../../analysis/analysis_ffi.dart';

const double analysisChartLabelW = 66.0;
const double analysisChartXAxisH = 34.0;
const double analysisChartSelectedFramePadding = 8.0;

void panChartByWheel({
  required double scrollDeltaY,
  required double viewStart,
  required double viewEnd,
  required double total,
  required ValueChanged<double> onPan,
}) {
  if (total <= 0) return;
  if (scrollDeltaY == 0) return;
  final span = (viewEnd - viewStart).clamp(1.0, total);
  final maxOffset = (total - span).clamp(0.0, double.infinity);
  if (maxOffset <= 0) return;
  final direction = scrollDeltaY > 0 ? 1.0 : -1.0;
  final step = (span * 0.15).clamp(1.0, 24.0);
  onPan((viewStart + direction * step).clamp(0.0, maxOffset));
}

String formatCompactAxisValue(int value) {
  final abs = value.abs();
  if (abs >= 1000000) return '${(value / 1000000).toStringAsFixed(1)}M';
  if (abs >= 1000) return '${(value / 1000).toStringAsFixed(1)}K';
  return '$value';
}

String formatBytes(int bytes) {
  if (bytes <= 0) return '0 B';
  if (bytes < 1024) return '$bytes B';
  final kb = bytes / 1024;
  if (kb < 1024) return '${kb.toStringAsFixed(1)} KB';
  final mb = kb / 1024;
  return '${mb.toStringAsFixed(1)} MB';
}

List<double> axisTickFractionsForHeight(
  double height, {
  int minTicks = 2,
  int maxTicks = 5,
  double minGap = 44.0,
}) {
  if (height <= 0) return const [];
  final tickCount = ((height / minGap).floor() + 1)
      .clamp(minTicks, maxTicks)
      .toInt();
  if (tickCount <= 1) return const [0.0];
  return [for (var i = 0; i < tickCount; i++) i / (tickCount - 1)];
}

void drawFrameXAxis({
  required Canvas canvas,
  required Size size,
  required double axisTop,
  required double labelW,
  required List<FrameInfo> frames,
  required int visibleStart,
  required int visibleEnd,
  required bool ptsOrder,
  required double Function(int frameIdx) xForFrame,
}) {
  if (axisTop >= size.height || visibleStart >= visibleEnd) return;

  final axisH = size.height - axisTop;
  if (axisH < 26) return;

  final plotLeft = labelW;
  final plotRight = size.width;
  final plotW = (plotRight - plotLeft).clamp(0.0, double.infinity);
  if (plotW <= 0) return;

  final axisPaint = Paint()
    ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.12)
    ..strokeWidth = 1.0;
  final tickPaint = Paint()
    ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.10)
    ..strokeWidth = 1.0;
  canvas.drawLine(Offset(0, axisTop), Offset(size.width, axisTop), axisPaint);
  canvas.drawLine(
    Offset(labelW, axisTop),
    Offset(labelW, size.height),
    axisPaint,
  );

  const labelStyle = TextStyle(
    color: Color(0xFFFFFFFF),
    fontSize: 10,
    fontWeight: FontWeight.w600,
  );
  const valueStyle = TextStyle(color: Color(0xCCFFFFFF), fontSize: 10);
  final axisName = ptsOrder ? 'PTS' : 'DTS';
  final leftTp = TextPainter(
    text: TextSpan(
      children: [
        const TextSpan(text: 'Index\n', style: labelStyle),
        TextSpan(text: axisName, style: valueStyle),
      ],
    ),
    textAlign: TextAlign.right,
    textDirection: TextDirection.ltr,
  )..layout(maxWidth: labelW - 8);
  leftTp.paint(canvas, Offset(labelW - leftTp.width - 6, axisTop + 4));

  final visibleCount = visibleEnd - visibleStart;
  final maxTicks = visibleCount == 1
      ? 1
      : (plotW / 92).floor().clamp(2, visibleCount);
  final step = (visibleCount / maxTicks).ceil().clamp(1, visibleCount);

  var lastRight = plotLeft - 8;
  for (var i = visibleStart; i < visibleEnd; i += step) {
    if (i < 0 || i >= frames.length) continue;
    final x = xForFrame(i);
    if (x < plotLeft || x > plotRight) continue;

    final f = frames[i];
    final value = ptsOrder ? f.pts : f.dts;
    final line1 = '#$i';
    final line2 = formatCompactAxisValue(value);
    final tickTp = TextPainter(
      text: TextSpan(
        children: [
          TextSpan(text: '$line1\n', style: labelStyle),
          TextSpan(text: line2, style: valueStyle),
        ],
      ),
      textAlign: TextAlign.center,
      textDirection: TextDirection.ltr,
    )..layout();
    final minDrawX = plotLeft + 2;
    final maxDrawX = (plotRight - tickTp.width - 2).clamp(
      minDrawX,
      double.infinity,
    );
    final drawX = (x - tickTp.width / 2).clamp(minDrawX, maxDrawX);
    if (drawX < lastRight + 8) continue;

    canvas.drawLine(Offset(x, axisTop), Offset(x, axisTop + 4), tickPaint);
    tickTp.paint(canvas, Offset(drawX, axisTop + 4));
    lastRight = drawX + tickTp.width;
  }
}

class AnalysisChartScrollbar extends StatefulWidget {
  final double total;
  final double viewStart;
  final double viewEnd;
  final ValueChanged<double> onPan;

  const AnalysisChartScrollbar({
    super.key,
    required this.total,
    required this.viewStart,
    required this.viewEnd,
    required this.onPan,
  });

  @override
  State<AnalysisChartScrollbar> createState() => AnalysisChartScrollbarState();
}

class AnalysisChartScrollbarState extends State<AnalysisChartScrollbar> {
  double _dragStart = 0;
  double _dragOffsetStart = 0;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final trackH = 8.0;
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onHorizontalDragStart: (details) {
        _dragStart = details.globalPosition.dx;
        _dragOffsetStart = widget.viewStart;
      },
      onHorizontalDragUpdate: (details) {
        final box = context.findRenderObject() as RenderBox;
        final trackW = box.size.width;
        if (trackW <= 0) return;
        final deltaPx = details.globalPosition.dx - _dragStart;
        final viewSpan = widget.viewEnd - widget.viewStart;
        final maxOffset = (widget.total - viewSpan).clamp(0.0, double.infinity);
        final newOffset = (_dragOffsetStart + deltaPx / trackW * widget.total)
            .clamp(0.0, maxOffset);
        widget.onPan(newOffset);
      },
      child: CustomPaint(
        painter: _ScrollbarPainter(
          total: widget.total,
          viewStart: widget.viewStart,
          viewEnd: widget.viewEnd,
          trackColor: theme.colorScheme.surfaceContainerHighest,
          thumbColor: theme.colorScheme.onSurface.withValues(alpha: 0.3),
          thumbHoverColor: theme.colorScheme.onSurface.withValues(alpha: 0.5),
        ),
        size: Size(Size.infinite.width, trackH),
      ),
    );
  }
}

class _ScrollbarPainter extends CustomPainter {
  final double total;
  final double viewStart;
  final double viewEnd;
  final Color trackColor;
  final Color thumbColor;
  final Color thumbHoverColor;

  _ScrollbarPainter({
    required this.total,
    required this.viewStart,
    required this.viewEnd,
    required this.trackColor,
    required this.thumbColor,
    required this.thumbHoverColor,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (total <= 0) return;
    const minThumbW = 44.0;

    // Thumb
    final rawLeft = (viewStart / total) * size.width;
    final rawRight = (viewEnd / total) * size.width;
    final rawW = (rawRight - rawLeft).clamp(0.0, size.width);
    final thumbW = size.width <= minThumbW
        ? size.width
        : rawW.clamp(minThumbW, size.width);
    final center = (rawLeft + rawRight) / 2;
    final left = (center - thumbW / 2).clamp(0.0, size.width - thumbW);
    canvas.drawRRect(
      RRect.fromRectAndRadius(
        Rect.fromLTWH(left, 0, thumbW, size.height),
        const Radius.circular(3),
      ),
      Paint()..color = thumbColor,
    );
  }

  @override
  bool shouldRepaint(covariant _ScrollbarPainter old) =>
      viewStart != old.viewStart ||
      viewEnd != old.viewEnd ||
      total != old.total;
}
