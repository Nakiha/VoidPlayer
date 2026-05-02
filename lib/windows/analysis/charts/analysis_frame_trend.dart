import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import '../../../analysis/analysis_ffi.dart';
import '../../../l10n/app_localizations.dart';
import '../page/analysis_page_state.dart';
import 'analysis_chart_common.dart';

// Frame Trend — zoomable / pannable bar chart
// ===========================================================================

const double _frameTrendLabelW = analysisChartLabelW;

class AnalysisFrameTrendView extends StatefulWidget {
  final List<FrameInfo> frames;
  final int frameIndexBase;
  final int totalFrames;
  final List<FrameBucket> frameBuckets;
  final int frameBucketSize;
  final int currentIdx;
  final int? selectedFrameIdx;
  final double viewStart;
  final double viewEnd;
  final double frameSizeAxisZoom;
  final double qpAxisZoom;
  final bool ptsOrder;
  final ValueChanged<double> onZoom;
  final void Function(AnalysisFrameTrendAxis axis, double scrollDelta)
  onAxisZoom;
  final ValueChanged<double> onPan;
  final ValueChanged<int?> onFrameSelected;
  final AppLocalizations l;
  const AnalysisFrameTrendView({
    super.key,
    required this.frames,
    this.frameIndexBase = 0,
    int? totalFrames,
    this.frameBuckets = const [],
    this.frameBucketSize = 1,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.viewStart,
    required this.viewEnd,
    required this.frameSizeAxisZoom,
    required this.qpAxisZoom,
    required this.ptsOrder,
    required this.onZoom,
    required this.onAxisZoom,
    required this.onPan,
    required this.onFrameSelected,
    required this.l,
  }) : totalFrames = totalFrames ?? frames.length;

  @override
  State<AnalysisFrameTrendView> createState() => _AnalysisFrameTrendViewState();
}

class _AnalysisFrameTrendViewState extends State<AnalysisFrameTrendView> {
  double? _hoverX; // null = not hovering

  @override
  Widget build(BuildContext context) {
    final w = widget;
    if (w.totalFrames == 0 || (w.frames.isEmpty && w.frameBuckets.isEmpty)) {
      return Center(child: Text(w.l.analysisNoFrameData));
    }
    return Column(
      children: [
        Expanded(
          child: Builder(
            builder: (chartContext) => MouseRegion(
              onExit: (_) => setState(() => _hoverX = null),
              child: Listener(
                onPointerSignal: (signal) {
                  if (signal is PointerScrollEvent) {
                    final box = chartContext.findRenderObject() as RenderBox;
                    final local = box.globalToLocal(signal.position);
                    final axisH = box.size.height >= 96
                        ? analysisChartXAxisH
                        : 0.0;
                    final chartH = (box.size.height - axisH).clamp(
                      1.0,
                      double.infinity,
                    );
                    final upperH = chartH * 0.58;
                    final lowerTop = upperH + chartH * 0.05;
                    final lowerH = chartH * 0.32;
                    final inPlotContent =
                        local.dx >= _frameTrendLabelW && local.dy < chartH;
                    if (inPlotContent) {
                      panChartByWheel(
                        scrollDeltaY: signal.scrollDelta.dy,
                        viewStart: w.viewStart,
                        viewEnd: w.viewEnd,
                        total: w.totalFrames.toDouble(),
                        onPan: w.onPan,
                      );
                    } else if (local.dx < _frameTrendLabelW) {
                      if (local.dy <= upperH) {
                        w.onAxisZoom(
                          AnalysisFrameTrendAxis.frameSize,
                          signal.scrollDelta.dy,
                        );
                      } else if (local.dy >= lowerTop &&
                          local.dy <= lowerTop + lowerH) {
                        w.onAxisZoom(
                          AnalysisFrameTrendAxis.qp,
                          signal.scrollDelta.dy,
                        );
                      } else {
                        w.onZoom(signal.scrollDelta.dy);
                      }
                    } else {
                      w.onZoom(signal.scrollDelta.dy);
                    }
                  }
                },
                child: GestureDetector(
                  behavior: HitTestBehavior.opaque,
                  onTapUp: (details) {
                    final box = chartContext.findRenderObject() as RenderBox;
                    final localX = box.globalToLocal(details.globalPosition).dx;
                    final chartW = box.size.width - _frameTrendLabelW;
                    if (chartW <= 0 || localX < _frameTrendLabelW) {
                      w.onFrameSelected(null);
                      return;
                    }
                    final span = w.viewEnd - w.viewStart;
                    final idx =
                        (w.viewStart +
                                ((localX - _frameTrendLabelW) / chartW) * span)
                            .round()
                            .clamp(0, w.totalFrames - 1)
                            .toInt();
                    w.onFrameSelected(w.selectedFrameIdx == idx ? null : idx);
                  },
                  onPanUpdate: (details) {
                    final box = chartContext.findRenderObject() as RenderBox;
                    final local = box.globalToLocal(details.globalPosition);
                    setState(() => _hoverX = local.dx);
                  },
                  child: MouseRegion(
                    onHover: (e) {
                      final box = chartContext.findRenderObject() as RenderBox;
                      final local = box.globalToLocal(e.position);
                      setState(() => _hoverX = local.dx);
                    },
                    child: CustomPaint(
                      painter: _FrameTrendPainter(
                        frames: w.frames,
                        frameIndexBase: w.frameIndexBase,
                        totalFrames: w.totalFrames,
                        frameBuckets: w.frameBuckets,
                        frameBucketSize: w.frameBucketSize,
                        currentIdx: w.currentIdx,
                        selectedFrameIdx: w.selectedFrameIdx,
                        viewStart: w.viewStart,
                        viewEnd: w.viewEnd,
                        frameSizeAxisZoom: w.frameSizeAxisZoom,
                        qpAxisZoom: w.qpAxisZoom,
                        ptsOrder: w.ptsOrder,
                        hoverX: _hoverX,
                      ),
                      size: Size.infinite,
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
        AnalysisChartScrollbar(
          total: w.totalFrames.toDouble(),
          viewStart: w.viewStart,
          viewEnd: w.viewEnd,
          onPan: w.onPan,
        ),
      ],
    );
  }
}

class _FrameTrendPainter extends CustomPainter {
  final List<FrameInfo> frames;
  final int frameIndexBase;
  final int totalFrames;
  final List<FrameBucket> frameBuckets;
  final int frameBucketSize;
  final int currentIdx;
  final int? selectedFrameIdx;
  final double viewStart;
  final double viewEnd;
  final double frameSizeAxisZoom;
  final double qpAxisZoom;
  final bool ptsOrder;
  final double? hoverX;

  _FrameTrendPainter({
    required this.frames,
    required this.frameIndexBase,
    required this.totalFrames,
    required this.frameBuckets,
    required this.frameBucketSize,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.viewStart,
    required this.viewEnd,
    required this.frameSizeAxisZoom,
    required this.qpAxisZoom,
    required this.ptsOrder,
    this.hoverX,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (totalFrames == 0 || (frames.isEmpty && frameBuckets.isEmpty)) return;

    final windowStart = frameIndexBase;
    final windowEnd = frameIndexBase + frames.length;
    final bucketMode = frameBuckets.isNotEmpty && frameBucketSize > 1;
    final visibleStart = bucketMode
        ? viewStart.floor().clamp(0, totalFrames - 1).toInt()
        : viewStart.floor().clamp(windowStart, windowEnd - 1).toInt();
    final visibleEnd = bucketMode
        ? (viewEnd.ceil() + 1).clamp(visibleStart + 1, totalFrames).toInt()
        : (viewEnd.ceil() + 1).clamp(windowStart, windowEnd).toInt();
    if (visibleStart >= visibleEnd) return;

    final visibleBuckets = bucketMode
        ? frameBuckets
              .where(
                (b) =>
                    b.frameCount > 0 &&
                    b.startFrame < visibleEnd &&
                    b.startFrame + b.frameCount > visibleStart,
              )
              .toList(growable: false)
        : const <FrameBucket>[];
    if (bucketMode && visibleBuckets.isEmpty) return;

    final count = bucketMode
        ? visibleBuckets.length
        : visibleEnd - visibleStart;
    final span = viewEnd - viewStart;

    final axisH = size.height >= 96 ? analysisChartXAxisH : 0.0;
    final chartH = (size.height - axisH).clamp(1.0, double.infinity);
    final labelW = _frameTrendLabelW;
    final chartW = (size.width - labelW).clamp(0.0, double.infinity);
    if (chartW <= 0) return;
    final contentPad = analysisChartSelectedFramePadding;
    final contentW = (chartW - contentPad * 2).clamp(1.0, double.infinity);
    final upperH = chartH * 0.58;
    final lowerH = chartH * 0.32;
    final gapH = chartH * 0.05;
    final lowerTop = upperH + gapH;
    final plotRect = Rect.fromLTWH(labelW, 0, chartW, chartH);

    final barW = (contentW / count).clamp(2.0, 40.0);
    double xForFrame(double frameIdx) {
      final frac = (frameIdx - viewStart) / span;
      return labelW + contentPad + frac * contentW;
    }

    // Find range for visible frames
    int maxPacketSize = 1;
    int minQp = 63, maxQp = 0;
    if (bucketMode) {
      for (final b in visibleBuckets) {
        if (b.packetSizeMax > maxPacketSize) {
          maxPacketSize = b.packetSizeMax;
        }
        if (b.qpMin < minQp) minQp = b.qpMin;
        if (b.qpMax > maxQp) maxQp = b.qpMax;
      }
    } else {
      for (var i = visibleStart; i < visibleEnd; i++) {
        final f = frames[i - frameIndexBase];
        if (f.packetSize > maxPacketSize) {
          maxPacketSize = f.packetSize;
        }
        if (f.avgQp < minQp) minQp = f.avgQp;
        if (f.avgQp > maxQp) maxQp = f.avgQp;
      }
    }
    final autoSizeMax = maxPacketSize.toDouble().clamp(1.0, double.infinity);
    final sizeAxisMax = (autoSizeMax / frameSizeAxisZoom).clamp(
      1.0,
      double.infinity,
    );

    final autoQpLow = (minQp / 5).floor() * 5.0;
    final autoQpHigh = ((maxQp / 5).floor() + 1) * 5.0;
    final autoQpRange = (autoQpHigh - autoQpLow).clamp(5.0, 63.0);
    final qpRange = (autoQpRange / qpAxisZoom).clamp(1.0, 63.0);
    final qpCenter = ((minQp + maxQp) / 2).clamp(0.0, 63.0);
    var qpLow = qpCenter - qpRange / 2;
    var qpHigh = qpCenter + qpRange / 2;
    if (qpLow < 0) {
      qpHigh -= qpLow;
      qpLow = 0;
    }
    if (qpHigh > 63) {
      qpLow -= qpHigh - 63;
      qpHigh = 63;
      if (qpLow < 0) qpLow = 0;
    }
    final effectiveQpRange = (qpHigh - qpLow).clamp(1.0, 63.0);

    // Label style — same as pyramid level labels
    const labelStyle = TextStyle(
      color: Color(0xFFFFFFFF),
      fontSize: 10,
      fontWeight: FontWeight.w600,
      letterSpacing: 0.5,
    );
    final axisPaint = Paint()
      ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.10)
      ..strokeWidth = 1.0;
    final gridPaint = Paint()
      ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.06)
      ..strokeWidth = 0.5;
    canvas.drawLine(Offset(labelW, 0), Offset(labelW, chartH), axisPaint);
    canvas.drawLine(
      Offset(0, upperH + gapH / 2),
      Offset(size.width, upperH + gapH / 2),
      Paint()
        ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.14)
        ..strokeWidth = 1.0,
    );

    // --- Packet size axis labels (upper) ---
    for (final yFrac in axisTickFractionsForHeight(upperH, maxTicks: 4)) {
      final text = formatBytes((sizeAxisMax * yFrac).round());
      final y = upperH * (1 - yFrac);
      canvas.drawLine(Offset(labelW, y), Offset(size.width, y), gridPaint);
      final tp = _axisLabelPainter(
        _byteAxisLabelLines(text),
        labelStyle,
        labelW - 8,
      );
      // Clamp to avoid clipping at top
      final drawY = (y - tp.height / 2).clamp(0.0, upperH - tp.height);
      tp.paint(canvas, Offset(labelW - 4 - tp.width, drawY));
    }

    // --- QP axis labels (lower) ---
    for (final yFrac in axisTickFractionsForHeight(lowerH, maxTicks: 4)) {
      final value = qpLow + effectiveQpRange * yFrac;
      final y = lowerTop + lowerH * (1 - yFrac);
      canvas.drawLine(Offset(labelW, y), Offset(size.width, y), gridPaint);
      final tp = TextPainter(
        text: TextSpan(text: _formatQpLabel(value), style: labelStyle),
        textDirection: TextDirection.ltr,
      )..layout();
      final drawY = (y - tp.height / 2).clamp(
        lowerTop,
        lowerTop + lowerH - tp.height,
      );
      tp.paint(canvas, Offset(labelW - 4 - tp.width, drawY));
    }

    canvas.save();
    canvas.clipRect(plotRect);

    // --- Frame size bars ---
    final barPaint = Paint()..style = PaintingStyle.fill;
    final selStroke = Paint()
      ..color = const Color(0xFFFFFFFF)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2.0;
    if (bucketMode) {
      for (final b in visibleBuckets) {
        final x = xForFrame(b.startFrame.toDouble());
        final x2 = xForFrame((b.startFrame + b.frameCount).toDouble());
        final w = (x2 - x).clamp(1.0, contentW);
        final h = ((b.avgPacketSize / sizeAxisMax).clamp(0.0, 1.0)) * upperH;

        barPaint.color = b.keyframeCount > 0
            ? const Color(0xFFFF5252)
            : const Color(0xFF42A5F5);
        final rect = Rect.fromLTWH(x, upperH - h, w - 1, h);
        canvas.drawRect(rect, barPaint);

        final selected = selectedFrameIdx;
        if (selected != null &&
            selected >= b.startFrame &&
            selected < b.startFrame + b.frameCount) {
          canvas.drawRect(rect.inflate(1), selStroke);
        }
      }
    } else {
      for (var i = visibleStart; i < visibleEnd; i++) {
        final f = frames[i - frameIndexBase];
        final x = xForFrame(i.toDouble());
        final h = ((f.packetSize / sizeAxisMax).clamp(0.0, 1.0)) * upperH;

        barPaint.color = f.keyframe == 1
            ? const Color(0xFFFF5252)
            : const Color(0xFF42A5F5);
        final rect = Rect.fromLTWH(x, upperH - h, barW - 1, h);
        canvas.drawRect(rect, barPaint);

        if (i == selectedFrameIdx) {
          canvas.drawRect(rect.inflate(1), selStroke);
        }
      }
    }

    // --- QP line ---
    final qpPaint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5
      ..color = const Color(0xFFFFB74D);
    final qpPath = Path();
    bool first = true;
    final qpSamples = bucketMode
        ? [
            for (final b in visibleBuckets)
              (x: xForFrame(b.startFrame + b.frameCount / 2), qp: b.avgQp),
          ]
        : [
            for (var i = visibleStart; i < visibleEnd; i++)
              (
                x: xForFrame(i.toDouble()) + barW / 2,
                qp: frames[i - frameIndexBase].avgQp.toDouble(),
              ),
          ];
    for (final sample in qpSamples) {
      final normalizedQp = ((sample.qp - qpLow) / effectiveQpRange).clamp(
        0.0,
        1.0,
      );
      final y = lowerTop + lowerH * (1 - normalizedQp);
      if (first) {
        qpPath.moveTo(sample.x, y);
        first = false;
      } else {
        qpPath.lineTo(sample.x, y);
      }
    }
    canvas.drawPath(qpPath, qpPaint);

    // --- Selection/playback cursor ---
    final cursorIdx = selectedFrameIdx != null && selectedFrameIdx! >= 0
        ? selectedFrameIdx!
        : currentIdx;
    if (cursorIdx >= visibleStart && cursorIdx < visibleEnd) {
      final cx = xForFrame(cursorIdx + 0.5);
      canvas.drawLine(
        Offset(cx, 0),
        Offset(cx, chartH),
        Paint()
          ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.5)
          ..strokeWidth = 1,
      );
    }

    // --- Hover crosshair + tooltip ---
    if (hoverX != null && hoverX! >= labelW) {
      final relX = hoverX! - labelW;
      final frameFrac = relX / chartW;
      final frameIdx = (viewStart + frameFrac * span)
          .round()
          .clamp(visibleStart, visibleEnd - 1)
          .toInt();

      final crossX = xForFrame(frameIdx + 0.5);
      canvas.drawLine(
        Offset(crossX, 0),
        Offset(crossX, chartH),
        Paint()
          ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.3)
          ..strokeWidth = 1,
      );

      final lines = bucketMode
          ? _bucketTooltipLines(visibleBuckets, frameIdx)
          : _frameTooltipLines(frames[frameIdx - frameIndexBase], frameIdx);
      final tipStyle = const TextStyle(color: Color(0xFFFFFFFF), fontSize: 10);
      final tipPainters = lines
          .map(
            (l) => TextPainter(
              text: TextSpan(text: l, style: tipStyle),
              textDirection: TextDirection.ltr,
            )..layout(),
          )
          .toList();
      final tipW =
          tipPainters.map((t) => t.width).reduce((a, b) => a > b ? a : b) + 12;
      final tipH = tipPainters.fold(0.0, (sum, t) => sum + t.height) + 10;

      var tipX = crossX + 8;
      if (tipX + tipW > size.width) tipX = crossX - tipW - 8;
      final tipY = 4.0;

      final bgPaint = Paint()..color = const Color(0xCC1A1A2E);
      final rrect = RRect.fromRectAndRadius(
        Rect.fromLTWH(tipX, tipY, tipW, tipH),
        const Radius.circular(4),
      );
      canvas.drawRRect(rrect, bgPaint);
      canvas.drawRRect(
        rrect,
        Paint()
          ..color = const Color(0x44FFFFFF)
          ..style = PaintingStyle.stroke
          ..strokeWidth = 0.5,
      );

      var offsetY = tipY + 5;
      for (final tp in tipPainters) {
        tp.paint(canvas, Offset(tipX + 6, offsetY));
        offsetY += tp.height;
      }
    }
    canvas.restore();

    if (bucketMode) {
      _drawFrameNumberXAxis(
        canvas: canvas,
        size: size,
        axisTop: chartH,
        labelW: labelW,
        visibleStart: visibleStart,
        visibleEnd: visibleEnd,
        xForFrame: (idx) => xForFrame(idx.toDouble() + 0.5),
      );
    } else {
      drawFrameXAxis(
        canvas: canvas,
        size: size,
        axisTop: chartH,
        labelW: labelW,
        frames: frames,
        visibleStart: visibleStart - frameIndexBase,
        visibleEnd: visibleEnd - frameIndexBase,
        ptsOrder: ptsOrder,
        xForFrame: (idx) {
          final frameIdx = idx + frameIndexBase;
          return xForFrame(frameIdx.toDouble()) + barW / 2;
        },
      );
    }
  }

  static List<String> _frameTooltipLines(FrameInfo f, int frameIdx) {
    final sliceLabel = switch (f.sliceType) {
      2 => 'I',
      1 => 'P',
      _ => f.numRefL1 > 0 ? 'B' : 'B(uni)',
    };
    return [
      '#$frameIdx  $sliceLabel  POC ${f.poc}',
      'Size: ${formatBytes(f.packetSize)}',
      'QP: ${f.avgQp}',
      'PTS: ${f.pts}',
      'DTS: ${f.dts}',
    ];
  }

  static List<String> _bucketTooltipLines(
    List<FrameBucket> buckets,
    int frameIdx,
  ) {
    final bucket = buckets.firstWhere(
      (b) => frameIdx >= b.startFrame && frameIdx < b.startFrame + b.frameCount,
      orElse: () => buckets.first,
    );
    final end = bucket.startFrame + bucket.frameCount - 1;
    return [
      '#${bucket.startFrame}-$end  ${bucket.frameCount} frames',
      'Avg size: ${formatBytes(bucket.avgPacketSize.round())}',
      'Max size: ${formatBytes(bucket.packetSizeMax)}',
      'QP avg: ${bucket.avgQp.toStringAsFixed(1)}',
      'QP range: ${bucket.qpMin}-${bucket.qpMax}',
      'Keyframes: ${bucket.keyframeCount}',
    ];
  }

  static void _drawFrameNumberXAxis({
    required Canvas canvas,
    required Size size,
    required double axisTop,
    required double labelW,
    required int visibleStart,
    required int visibleEnd,
    required double Function(int idx) xForFrame,
  }) {
    if (size.height < 96 || visibleEnd <= visibleStart) return;
    const style = TextStyle(color: Color(0x99FFFFFF), fontSize: 10);
    final axisPaint = Paint()
      ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.10)
      ..strokeWidth = 1.0;
    canvas.drawLine(
      Offset(labelW, axisTop),
      Offset(size.width, axisTop),
      axisPaint,
    );
    final tickCount = ((size.width - labelW) / 96).floor().clamp(2, 8).toInt();
    final span = visibleEnd - visibleStart;
    for (var i = 0; i <= tickCount; i++) {
      final frameIdx = (visibleStart + span * i / tickCount).round();
      final x = xForFrame(frameIdx).clamp(labelW, size.width);
      canvas.drawLine(Offset(x, axisTop), Offset(x, axisTop + 4), axisPaint);
      final tp = TextPainter(
        text: TextSpan(text: '#$frameIdx', style: style),
        textDirection: TextDirection.ltr,
      )..layout();
      tp.paint(
        canvas,
        Offset(
          (x - tp.width / 2).clamp(labelW, size.width - tp.width),
          axisTop + 5,
        ),
      );
    }
  }

  static TextPainter _axisLabelPainter(
    List<String> lines,
    TextStyle style,
    double maxWidth,
  ) {
    return TextPainter(
      text: TextSpan(text: lines.join('\n'), style: style),
      textAlign: TextAlign.right,
      textDirection: TextDirection.ltr,
      maxLines: lines.length,
    )..layout(maxWidth: maxWidth);
  }

  static List<String> _byteAxisLabelLines(String text) {
    final parts = text.split(' ');
    if (parts.length == 2 && parts[0] != '0') {
      return [parts[0], parts[1]];
    }
    return [text];
  }

  static String _formatQpLabel(double value) {
    final rounded = value.roundToDouble();
    if ((value - rounded).abs() < 0.05) return '${rounded.toInt()}';
    return value.toStringAsFixed(1);
  }

  @override
  bool shouldRepaint(covariant _FrameTrendPainter old) =>
      frames.length != old.frames.length ||
      frameIndexBase != old.frameIndexBase ||
      totalFrames != old.totalFrames ||
      frameBuckets.length != old.frameBuckets.length ||
      frameBucketSize != old.frameBucketSize ||
      currentIdx != old.currentIdx ||
      selectedFrameIdx != old.selectedFrameIdx ||
      viewStart != old.viewStart ||
      viewEnd != old.viewEnd ||
      frameSizeAxisZoom != old.frameSizeAxisZoom ||
      qpAxisZoom != old.qpAxisZoom ||
      ptsOrder != old.ptsOrder ||
      hoverX != old.hoverX;
}

// ===========================================================================
// Shared chart scrollbar
// ===========================================================================
