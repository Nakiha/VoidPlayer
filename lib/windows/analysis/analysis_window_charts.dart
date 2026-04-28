import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import '../../analysis/analysis_ffi.dart';
import '../../l10n/app_localizations.dart';

// ===========================================================================
// Reference Pyramid — circle nodes + reference arrows + level backgrounds
// Supports zoom (scroll wheel) and pan (scrollbar).
// ===========================================================================

const double _analysisChartLabelW = 66.0;
const double _analysisChartXAxisH = 34.0;

String _formatCompactAxisValue(int value) {
  final abs = value.abs();
  if (abs >= 1000000) return '${(value / 1000000).toStringAsFixed(1)}M';
  if (abs >= 1000) return '${(value / 1000).toStringAsFixed(1)}K';
  return '$value';
}

List<double> _axisTickFractionsForHeight(
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

void _drawFrameXAxis({
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
    final line2 = _formatCompactAxisValue(value);
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

class AnalysisReferencePyramidView extends StatefulWidget {
  final List<FrameInfo> frames;
  final int currentIdx;
  final int? selectedFrameIdx;
  final Map<int, List<int>> pocToIndices;
  final ValueChanged<int?> onFrameSelected;
  final double viewStart;
  final double viewEnd;
  final bool ptsOrder;
  final ValueChanged<double> onZoom;
  final ValueChanged<double> onPan;
  final AppLocalizations l;
  const AnalysisReferencePyramidView({
    super.key,
    required this.frames,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.pocToIndices,
    required this.onFrameSelected,
    required this.viewStart,
    required this.viewEnd,
    required this.ptsOrder,
    required this.onZoom,
    required this.onPan,
    required this.l,
  });

  @override
  State<AnalysisReferencePyramidView> createState() =>
      _AnalysisReferencePyramidViewState();
}

class _AnalysisReferencePyramidViewState
    extends State<AnalysisReferencePyramidView> {
  _RefPyramidPainter? _lastPainter;
  Offset? _hoverPosition;

  @override
  Widget build(BuildContext context) {
    if (widget.frames.isEmpty) {
      return Center(child: Text(widget.l.analysisNoFrameData));
    }
    return Column(
      children: [
        Expanded(
          child: Listener(
            onPointerSignal: (signal) {
              if (signal is PointerScrollEvent) {
                widget.onZoom(signal.scrollDelta.dy);
              }
            },
            child: GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTapUp: (details) {
                final rects = _lastPainter?.frameRects ?? [];
                for (final (gi, rect) in rects) {
                  if (rect.contains(details.localPosition)) {
                    widget.onFrameSelected(
                      widget.selectedFrameIdx == gi ? null : gi,
                    );
                    return;
                  }
                }
                widget.onFrameSelected(null);
              },
              child: Builder(
                builder: (chartContext) => MouseRegion(
                  onExit: (_) => setState(() => _hoverPosition = null),
                  onHover: (event) {
                    final box = chartContext.findRenderObject() as RenderBox;
                    setState(() {
                      _hoverPosition = box.globalToLocal(event.position);
                    });
                  },
                  child: CustomPaint(
                    painter: _lastPainter = _RefPyramidPainter(
                      frames: widget.frames,
                      currentIdx: widget.currentIdx,
                      selectedFrameIdx: widget.selectedFrameIdx,
                      pocToIndices: widget.pocToIndices,
                      viewStart: widget.viewStart,
                      viewEnd: widget.viewEnd,
                      ptsOrder: widget.ptsOrder,
                      hoverPosition: _hoverPosition,
                    ),
                    size: Size.infinite,
                  ),
                ),
              ),
            ),
          ),
        ),
        _ChartScrollbar(
          total: widget.frames.length.toDouble(),
          viewStart: widget.viewStart,
          viewEnd: widget.viewEnd,
          onPan: widget.onPan,
        ),
      ],
    );
  }
}

class _RefPyramidPainter extends CustomPainter {
  final List<FrameInfo> frames;
  final int currentIdx;
  final int? selectedFrameIdx;
  final Map<int, List<int>> pocToIndices;
  final double viewStart;
  final double viewEnd;
  final bool ptsOrder;
  final Offset? hoverPosition;

  /// Populated during paint() — used by parent for hit-testing.
  List<(int, Rect)> frameRects = [];

  _RefPyramidPainter({
    required this.frames,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.pocToIndices,
    required this.viewStart,
    required this.viewEnd,
    required this.ptsOrder,
    this.hoverPosition,
  });

  // Pre-allocated Paint objects — reused across draw calls within paint()
  static final _bgPaint = Paint();
  static final _cursorPaint = Paint();
  static final _arrowLinePaint = Paint()
    ..style = PaintingStyle.stroke
    ..strokeCap = StrokeCap.round;
  static final _arrowHeadPaint = Paint();
  static final _fillPaint = Paint();
  static final _strokePaint = Paint()..style = PaintingStyle.stroke;

  @override
  void paint(Canvas canvas, Size size) {
    if (frames.isEmpty) return;

    final visibleStart = viewStart.floor().clamp(0, frames.length - 1);
    final visibleEnd = (viewEnd.ceil() + 1).clamp(0, frames.length);
    if (visibleStart >= visibleEnd) return;

    // --- Layout ---
    int maxTid = 0;
    for (var i = visibleStart; i < visibleEnd; i++) {
      if (frames[i].temporalId > maxTid) maxTid = frames[i].temporalId;
    }
    final axisH = size.height >= 96 ? _analysisChartXAxisH : 0.0;
    final chartH = (size.height - axisH).clamp(1.0, double.infinity);
    final numLevels = maxTid + 1;
    final rowH = chartH / numLevels;
    final labelW = _analysisChartLabelW;
    final usableW = (size.width - labelW).clamp(0.0, double.infinity);
    final span = viewEnd - viewStart;
    final circleR = (rowH * 0.3).clamp(6.0, 20.0);
    final plotRect = Rect.fromLTWH(labelW, 0, usableW, chartH);
    final centerPad = circleR + 2;
    final centerW = (usableW - centerPad * 2).clamp(1.0, double.infinity);

    final positions = <int, Offset>{}; // globalIdx → position
    for (var i = visibleStart; i < visibleEnd; i++) {
      final frac = (i - viewStart) / span;
      final x = labelW + centerPad + frac * centerW;
      final y = chartH - (frames[i].temporalId + 0.5) * rowH;
      positions[i] = Offset(x, y);
    }
    frameRects = [
      for (final e in positions.entries) ...[
        if (Rect.fromCircle(
          center: e.value,
          radius: circleR,
        ).overlaps(plotRect))
          (
            e.key,
            Rect.fromCircle(
              center: e.value,
              radius: circleR,
            ).intersect(plotRect),
          ),
      ],
    ];

    // --- Level backgrounds ---
    final levelLabelStep = rowH >= 16 ? 1 : (16 / rowH).ceil();
    for (var tid = 0; tid <= maxTid; tid++) {
      final top = chartH - (tid + 1) * rowH;
      final alpha = 0.03 + (maxTid - tid) * 0.025;
      _bgPaint.color = const Color(0xFFFFFFFF).withValues(alpha: alpha);
      canvas.drawRect(Rect.fromLTWH(0, top, size.width, rowH), _bgPaint);
      final showLevelLabel =
          tid == 0 || tid == maxTid || tid % levelLabelStep == 0;
      if (!showLevelLabel) continue;
      final tp = TextPainter(
        text: TextSpan(
          text: 'L$tid',
          style: const TextStyle(
            color: Color(0xFFFFFFFF),
            fontSize: 10,
            fontWeight: FontWeight.w600,
            letterSpacing: 0.5,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      final drawY = (top + rowH / 2 - tp.height / 2).clamp(
        0.0,
        chartH - tp.height,
      );
      tp.paint(canvas, Offset(4, drawY));
    }
    canvas.drawLine(
      Offset(labelW, 0),
      Offset(labelW, chartH),
      Paint()
        ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.10)
        ..strokeWidth = 1,
    );

    canvas.save();
    canvas.clipRect(plotRect);
    // --- Current playback cursor ---
    if (currentIdx >= 0 && positions.containsKey(currentIdx)) {
      final cx = positions[currentIdx]!.dx;
      _cursorPaint.color = const Color(0xFFFFFFFF).withValues(alpha: 0.15);
      _cursorPaint.strokeWidth = 1;
      canvas.drawLine(Offset(cx, 0), Offset(cx, chartH), _cursorPaint);
    }

    // --- Reference arrows ---
    // pocToIndices is pre-built by parent (cached across repaints).

    int? nearestRefIdx(int refPoc, int sourceIdx) {
      final indices = pocToIndices[refPoc];
      if (indices == null || indices.isEmpty) return null;
      var best = indices[0];
      for (final idx in indices) {
        if ((idx - sourceIdx).abs() < (best - sourceIdx).abs()) best = idx;
      }
      return best;
    }

    Offset posFor(int idx) {
      if (positions.containsKey(idx)) return positions[idx]!;
      final frac = (idx - viewStart) / span;
      final x = labelW + centerPad + frac * centerW;
      final y = chartH - (frames[idx].temporalId + 0.5) * rowH;
      return Offset(x, y);
    }

    bool endpointVisible(int idx) {
      if (idx < 0 || idx >= frames.length) return false;
      return Rect.fromCircle(
        center: posFor(idx),
        radius: circleR,
      ).overlaps(plotRect);
    }

    List<int> refsFor(int sourceIdx) {
      final f = frames[sourceIdx];
      final refs = <int>[];
      for (var j = 0; j < f.numRefL0 && j < f.refPocsL0.length; j++) {
        final ri = nearestRefIdx(f.refPocsL0[j], sourceIdx);
        if (ri != null && ri >= 0 && ri < frames.length) refs.add(ri);
      }
      for (var j = 0; j < f.numRefL1 && j < f.refPocsL1.length; j++) {
        final ri = nearestRefIdx(f.refPocsL1[j], sourceIdx);
        if (ri != null && ri >= 0 && ri < frames.length) refs.add(ri);
      }
      return refs;
    }

    // Collect the selected frame's transitive reference chain for highlighting.
    final selectedChainEdges = <String>{};
    final selectedChainNodes = <int>{};
    if (selectedFrameIdx != null &&
        selectedFrameIdx! >= 0 &&
        selectedFrameIdx! < frames.length) {
      void visitReferenceChain(int sourceIdx) {
        if (!selectedChainNodes.add(sourceIdx)) return;
        for (final refIdx in refsFor(sourceIdx)) {
          selectedChainEdges.add('$sourceIdx:$refIdx');
          visitReferenceChain(refIdx);
        }
      }

      visitReferenceChain(selectedFrameIdx!);
    }

    // Helper: get fill color for a frame's circle (same logic as circle drawing)
    Color frameFillColor(FrameInfo f) {
      if (f.sliceType == 2) return const Color(0xFFFF4D4F); // I: red
      if (f.sliceType == 0 && f.numRefL1 > 0) {
        return const Color(0xFF1890FF); // B bidir: blue
      }
      return const Color(0xFF52C41A); // B uni / P: green
    }

    // Draw reference edges that affect the viewport. This includes offscreen
    // sources/targets when the other endpoint is visible, preventing lines from
    // popping in only when an offscreen source node enters the viewport.
    final drawnEdges = <String>{};
    for (var i = 0; i < frames.length; i++) {
      final f = frames[i];
      if (f.numRefL0 == 0 && f.numRefL1 == 0) continue;
      final from = posFor(i);
      final sourceVisible = endpointVisible(i);

      for (final ri in refsFor(i)) {
        final edgeKey = '$i:$ri';
        if (!drawnEdges.add(edgeKey)) continue;

        final to = posFor(ri);
        final targetVisible = endpointVisible(ri);
        final isSelLine =
            selectedFrameIdx != null && selectedChainEdges.contains(edgeKey);
        if (!sourceVisible && !targetVisible && !isSelLine) continue;
        if (!_segmentIntersectsRect(from, to, plotRect)) continue;

        final lineW = isSelLine
            ? 2.5
            : (sourceVisible && targetVisible ? 1 : 0.8);
        final baseAlpha = sourceVisible && targetVisible ? 0.5 : 0.36;
        final arrowAlpha = isSelLine
            ? 1.0
            : (selectedFrameIdx != null ? baseAlpha * 0.4 : baseAlpha);
        final arrowColor = frameFillColor(
          frames[ri],
        ).withValues(alpha: arrowAlpha);
        _drawArrow(canvas, from, to, arrowColor, lineW.toDouble(), circleR);
      }
    }
    // --- Frame circles ---
    final related = <int>{};
    if (selectedFrameIdx != null) {
      related.addAll(selectedChainNodes);
    }

    // Cache label TextPainters (only 3 variants: I, P, B)
    TextPainter? labelI, labelP, labelB;
    if (circleR >= 8) {
      labelI = TextPainter(
        text: const TextSpan(
          text: 'I',
          style: TextStyle(
            color: Color(0xFFFFFFFF),
            fontSize: 14,
            fontWeight: FontWeight.bold,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      labelP = TextPainter(
        text: const TextSpan(
          text: 'P',
          style: TextStyle(
            color: Color(0xFFFFFFFF),
            fontSize: 14,
            fontWeight: FontWeight.bold,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      labelB = TextPainter(
        text: const TextSpan(
          text: 'B',
          style: TextStyle(
            color: Color(0xFF0050B3),
            fontSize: 14,
            fontWeight: FontWeight.bold,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
    }

    for (final entry in positions.entries) {
      final i = entry.key;
      final pos = entry.value;
      final f = frames[i];
      final isSelected = i == selectedFrameIdx;
      final isRelated = related.contains(i);

      // VBS2 slice_type: 0=B, 1=P, 2=I (see binary_types.h)
      // B with l1=0 (unidirectional) uses green color but still labeled B
      final Color fill, stroke;
      if (f.sliceType == 2) {
        fill = const Color(0xFFFF4D4F);
        stroke = const Color(0xFFCF1322);
      } else if (f.sliceType == 0 && f.numRefL1 > 0) {
        fill = const Color(0xFFE6F7FF);
        stroke = const Color(0xFF1890FF);
      } else if (f.sliceType == 0) {
        // B with l1==0 (unidirectional): green color
        fill = const Color(0xFF52C41A);
        stroke = const Color(0xFF389E0D);
      } else {
        // P (sliceType==1)
        fill = const Color(0xFF52C41A);
        stroke = const Color(0xFF389E0D);
      }

      final sw2 = isSelected ? 4.5 : (isRelated ? 3.5 : 2.5);
      final r = isSelected ? circleR + 2 : circleR;

      _fillPaint.color = fill;
      canvas.drawCircle(pos, r, _fillPaint);
      _strokePaint.color = stroke;
      _strokePaint.strokeWidth = sw2;
      canvas.drawCircle(pos, r, _strokePaint);
      if (isSelected) {
        _strokePaint.color = const Color(0xFFFFFFFF);
        _strokePaint.strokeWidth = 2.0;
        canvas.drawCircle(pos, r + sw2 / 2 + 1.5, _strokePaint);
      }

      if (circleR >= 8) {
        // Label: always show actual slice type (I/P/B), color distinguishes ref direction
        final TextPainter tp;
        if (f.sliceType == 2) {
          tp = labelI!;
        } else if (f.sliceType == 0) {
          tp = labelB!;
        } else {
          tp = labelP!;
        }
        tp.paint(canvas, pos - Offset(tp.width / 2, tp.height / 2));
      }
    }
    _drawHoverTooltip(canvas, size, plotRect, positions, circleR);
    canvas.restore();

    _drawFrameXAxis(
      canvas: canvas,
      size: size,
      axisTop: chartH,
      labelW: labelW,
      frames: frames,
      visibleStart: visibleStart,
      visibleEnd: visibleEnd,
      ptsOrder: ptsOrder,
      xForFrame: (idx) {
        final frac = (idx - viewStart) / span;
        return labelW + centerPad + frac * centerW;
      },
    );
  }

  void _drawHoverTooltip(
    Canvas canvas,
    Size size,
    Rect plotRect,
    Map<int, Offset> positions,
    double circleR,
  ) {
    final hover = hoverPosition;
    if (hover == null || !plotRect.contains(hover)) return;

    int? frameIdx;
    for (final (idx, rect) in frameRects) {
      if (rect.contains(hover)) {
        frameIdx = idx;
        break;
      }
    }
    if (frameIdx == null || frameIdx < 0 || frameIdx >= frames.length) return;

    final f = frames[frameIdx];
    final pos = positions[frameIdx] ?? hover;
    final sliceLabel = switch (f.sliceType) {
      2 => 'I',
      1 => 'P',
      _ => f.numRefL1 > 0 ? 'B' : 'B(uni)',
    };
    final lines = [
      '#$frameIdx  $sliceLabel  POC ${f.poc}',
      'Size: ${_FrameTrendPainter._formatBytes(f.packetSize)}',
      'QP: ${f.avgQp}',
      'PTS: ${f.pts}',
      'DTS: ${f.dts}',
    ];
    const tipStyle = TextStyle(color: Color(0xFFFFFFFF), fontSize: 10);
    final tipPainters = lines
        .map(
          (line) => TextPainter(
            text: TextSpan(text: line, style: tipStyle),
            textDirection: TextDirection.ltr,
          )..layout(),
        )
        .toList();
    final tipW =
        tipPainters.map((t) => t.width).reduce((a, b) => a > b ? a : b) + 12;
    final tipH = tipPainters.fold(0.0, (sum, t) => sum + t.height) + 10;

    var tipX = pos.dx + circleR + 8;
    if (tipX + tipW > plotRect.right) tipX = pos.dx - circleR - tipW - 8;
    final minTipX = plotRect.left + 4;
    final maxTipX = (plotRect.right - tipW - 4).clamp(minTipX, double.infinity);
    tipX = tipX.clamp(minTipX, maxTipX);
    final maxTipY = (size.height - tipH - 4).clamp(4.0, double.infinity);
    final tipY = (pos.dy - tipH - circleR - 8).clamp(4.0, maxTipY);

    final rrect = RRect.fromRectAndRadius(
      Rect.fromLTWH(tipX, tipY, tipW, tipH),
      const Radius.circular(4),
    );
    canvas.drawRRect(rrect, Paint()..color = const Color(0xCC1A1A2E));
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

  void _drawArrow(
    Canvas canvas,
    Offset from,
    Offset to,
    Color color,
    double strokeWidth,
    double circleR,
  ) {
    final delta = to - from;
    final dist = delta.distance;
    if (dist < circleR * 2 + 2) return;

    final unit = delta / dist;
    final p1 = from + unit * circleR;
    final p2 = to - unit * (circleR + 6);

    _arrowLinePaint.color = color;
    _arrowLinePaint.strokeWidth = strokeWidth;
    canvas.drawLine(p1, p2, _arrowLinePaint);

    final arrowLen = strokeWidth * 3.5;
    final perp = Offset(-unit.dy, unit.dx);
    final tip = to - unit * circleR;
    final a1 = tip - unit * arrowLen + perp * arrowLen * 0.45;
    final a2 = tip - unit * arrowLen - perp * arrowLen * 0.45;

    _arrowHeadPaint.color = color;
    canvas.drawPath(
      Path()
        ..moveTo(tip.dx, tip.dy)
        ..lineTo(a1.dx, a1.dy)
        ..lineTo(a2.dx, a2.dy)
        ..close(),
      _arrowHeadPaint,
    );
  }

  bool _segmentIntersectsRect(Offset a, Offset b, Rect rect) {
    if (rect.contains(a) || rect.contains(b)) return true;
    final left = a.dx < b.dx ? a.dx : b.dx;
    final right = a.dx > b.dx ? a.dx : b.dx;
    final top = a.dy < b.dy ? a.dy : b.dy;
    final bottom = a.dy > b.dy ? a.dy : b.dy;
    if (right < rect.left ||
        left > rect.right ||
        bottom < rect.top ||
        top > rect.bottom) {
      return false;
    }

    final topLeft = rect.topLeft;
    final topRight = rect.topRight;
    final bottomRight = rect.bottomRight;
    final bottomLeft = rect.bottomLeft;
    return _segmentsIntersect(a, b, topLeft, topRight) ||
        _segmentsIntersect(a, b, topRight, bottomRight) ||
        _segmentsIntersect(a, b, bottomRight, bottomLeft) ||
        _segmentsIntersect(a, b, bottomLeft, topLeft);
  }

  bool _segmentsIntersect(Offset a, Offset b, Offset c, Offset d) {
    final o1 = _orientation(a, b, c);
    final o2 = _orientation(a, b, d);
    final o3 = _orientation(c, d, a);
    final o4 = _orientation(c, d, b);
    if (o1 == 0 && _onSegment(a, c, b)) return true;
    if (o2 == 0 && _onSegment(a, d, b)) return true;
    if (o3 == 0 && _onSegment(c, a, d)) return true;
    if (o4 == 0 && _onSegment(c, b, d)) return true;
    return (o1 > 0) != (o2 > 0) && (o3 > 0) != (o4 > 0);
  }

  double _orientation(Offset a, Offset b, Offset c) {
    final v = (b.dy - a.dy) * (c.dx - b.dx) - (b.dx - a.dx) * (c.dy - b.dy);
    return v.abs() < 0.0001 ? 0 : v;
  }

  bool _onSegment(Offset a, Offset b, Offset c) {
    return b.dx <= (a.dx > c.dx ? a.dx : c.dx) + 0.0001 &&
        b.dx + 0.0001 >= (a.dx < c.dx ? a.dx : c.dx) &&
        b.dy <= (a.dy > c.dy ? a.dy : c.dy) + 0.0001 &&
        b.dy + 0.0001 >= (a.dy < c.dy ? a.dy : c.dy);
  }

  @override
  bool shouldRepaint(covariant _RefPyramidPainter old) =>
      frames.length != old.frames.length ||
      currentIdx != old.currentIdx ||
      selectedFrameIdx != old.selectedFrameIdx ||
      viewStart != old.viewStart ||
      viewEnd != old.viewEnd ||
      ptsOrder != old.ptsOrder ||
      hoverPosition != old.hoverPosition;
}

// ===========================================================================
// Frame Trend — zoomable / pannable bar chart
// ===========================================================================

const double _frameTrendLabelW = _analysisChartLabelW;

enum AnalysisFrameTrendAxis { frameSize, qp }

class AnalysisFrameTrendView extends StatefulWidget {
  final List<FrameInfo> frames;
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
  });

  @override
  State<AnalysisFrameTrendView> createState() => _AnalysisFrameTrendViewState();
}

class _AnalysisFrameTrendViewState extends State<AnalysisFrameTrendView> {
  double? _hoverX; // null = not hovering

  @override
  Widget build(BuildContext context) {
    final w = widget;
    if (w.frames.isEmpty) {
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
                        ? _analysisChartXAxisH
                        : 0.0;
                    final chartH = (box.size.height - axisH).clamp(
                      1.0,
                      double.infinity,
                    );
                    final upperH = chartH * 0.58;
                    final lowerTop = upperH + chartH * 0.05;
                    final lowerH = chartH * 0.32;
                    if (local.dx < _frameTrendLabelW) {
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
                            .clamp(0, w.frames.length - 1);
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
        _ChartScrollbar(
          total: w.frames.length.toDouble(),
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
    if (frames.isEmpty) return;

    final visibleStart = viewStart.floor().clamp(0, frames.length - 1);
    final visibleEnd = (viewEnd.ceil() + 1).clamp(0, frames.length);
    if (visibleStart >= visibleEnd) return;

    final count = visibleEnd - visibleStart;
    final span = viewEnd - viewStart;

    final axisH = size.height >= 96 ? _analysisChartXAxisH : 0.0;
    final chartH = (size.height - axisH).clamp(1.0, double.infinity);
    final labelW = _frameTrendLabelW;
    final chartW = (size.width - labelW).clamp(0.0, double.infinity);
    if (chartW <= 0) return;
    final upperH = chartH * 0.58;
    final lowerH = chartH * 0.32;
    final gapH = chartH * 0.05;
    final lowerTop = upperH + gapH;
    final plotRect = Rect.fromLTWH(labelW, 0, chartW, chartH);

    final barW = (chartW / count).clamp(2.0, 40.0);

    // Find range for visible frames
    int maxPacketSize = 1;
    int minQp = 63, maxQp = 0;
    for (var i = visibleStart; i < visibleEnd; i++) {
      if (frames[i].packetSize > maxPacketSize) {
        maxPacketSize = frames[i].packetSize;
      }
      if (frames[i].avgQp < minQp) minQp = frames[i].avgQp;
      if (frames[i].avgQp > maxQp) maxQp = frames[i].avgQp;
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
    for (final yFrac in _axisTickFractionsForHeight(upperH, maxTicks: 4)) {
      final text = _formatBytes((sizeAxisMax * yFrac).round());
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
    for (final yFrac in _axisTickFractionsForHeight(lowerH, maxTicks: 4)) {
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
    for (var i = visibleStart; i < visibleEnd; i++) {
      final f = frames[i];
      final frac = (i - viewStart) / span;
      final x = labelW + frac * chartW;
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

    // --- QP line ---
    final qpPaint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5
      ..color = const Color(0xFFFFB74D);
    final qpPath = Path();
    bool first = true;
    for (var i = visibleStart; i < visibleEnd; i++) {
      final f = frames[i];
      final frac = (i - viewStart) / span;
      final x = labelW + frac * chartW + barW / 2;
      final normalizedQp = ((f.avgQp - qpLow) / effectiveQpRange).clamp(
        0.0,
        1.0,
      );
      final y = lowerTop + lowerH * (1 - normalizedQp);
      if (first) {
        qpPath.moveTo(x, y);
        first = false;
      } else {
        qpPath.lineTo(x, y);
      }
    }
    canvas.drawPath(qpPath, qpPaint);

    // --- Playback cursor ---
    if (currentIdx >= 0 && currentIdx < frames.length) {
      final frac = (currentIdx - viewStart) / span;
      final cx = labelW + frac * chartW;
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
      final frameIdx = (viewStart + frameFrac * span).round().clamp(
        visibleStart,
        visibleEnd - 1,
      );

      final crossX =
          labelW + ((frameIdx - viewStart) / span) * chartW + barW / 2;
      canvas.drawLine(
        Offset(crossX, 0),
        Offset(crossX, chartH),
        Paint()
          ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.3)
          ..strokeWidth = 1,
      );

      final f = frames[frameIdx];
      final sliceLabel = switch (f.sliceType) {
        2 => 'I',
        1 => 'P',
        _ => f.numRefL1 > 0 ? 'B' : 'B(uni)',
      };
      final lines = [
        '#$frameIdx  $sliceLabel  POC ${f.poc}',
        'Size: ${_formatBytes(f.packetSize)}',
        'QP: ${f.avgQp}',
        'PTS: ${f.pts}',
        'DTS: ${f.dts}',
      ];
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

    _drawFrameXAxis(
      canvas: canvas,
      size: size,
      axisTop: chartH,
      labelW: labelW,
      frames: frames,
      visibleStart: visibleStart,
      visibleEnd: visibleEnd,
      ptsOrder: ptsOrder,
      xForFrame: (idx) {
        final frac = (idx - viewStart) / span;
        return labelW + frac * chartW + barW / 2;
      },
    );
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

  static String _formatBytes(int bytes) {
    if (bytes <= 0) return '0 B';
    if (bytes < 1024) return '$bytes B';
    final kb = bytes / 1024;
    if (kb < 1024) return '${kb.toStringAsFixed(1)} KB';
    final mb = kb / 1024;
    return '${mb.toStringAsFixed(1)} MB';
  }

  @override
  bool shouldRepaint(covariant _FrameTrendPainter old) =>
      frames.length != old.frames.length ||
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

class _ChartScrollbar extends StatefulWidget {
  final double total;
  final double viewStart;
  final double viewEnd;
  final ValueChanged<double> onPan;

  const _ChartScrollbar({
    required this.total,
    required this.viewStart,
    required this.viewEnd,
    required this.onPan,
  });

  @override
  State<_ChartScrollbar> createState() => _ChartScrollbarState();
}

class _ChartScrollbarState extends State<_ChartScrollbar> {
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

    // Track
    canvas.drawRect(
      Rect.fromLTWH(0, size.height - 3, size.width, 3),
      Paint()..color = trackColor,
    );

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
