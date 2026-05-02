import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import '../../../analysis/analysis_ffi.dart';
import '../../../l10n/app_localizations.dart';
import 'analysis_chart_common.dart';

// Reference pyramid chart: circle nodes plus reference arrows.

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
  Offset? _hoverPosition;
  late _FrameReferenceCache _referenceCache;
  int? _cachedSelectedFrameIdx;
  Set<String> _selectedChainEdges = const {};
  Set<int> _selectedChainNodes = const {};

  @override
  void initState() {
    super.initState();
    _referenceCache = _FrameReferenceCache.build(
      widget.frames,
      widget.pocToIndices,
    );
    _rebuildSelectedChain();
  }

  @override
  void didUpdateWidget(covariant AnalysisReferencePyramidView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!identical(widget.frames, oldWidget.frames) ||
        !identical(widget.pocToIndices, oldWidget.pocToIndices)) {
      _referenceCache = _FrameReferenceCache.build(
        widget.frames,
        widget.pocToIndices,
      );
      _cachedSelectedFrameIdx = null;
    }
    if (widget.selectedFrameIdx != oldWidget.selectedFrameIdx ||
        _cachedSelectedFrameIdx != widget.selectedFrameIdx) {
      _rebuildSelectedChain();
    }
  }

  void _rebuildSelectedChain() {
    _cachedSelectedFrameIdx = widget.selectedFrameIdx;
    final selectedFrameIdx = widget.selectedFrameIdx;
    if (selectedFrameIdx == null ||
        selectedFrameIdx < 0 ||
        selectedFrameIdx >= widget.frames.length) {
      _selectedChainEdges = const {};
      _selectedChainNodes = const {};
      return;
    }

    final selectedChainEdges = <String>{};
    final selectedChainNodes = <int>{};
    final stack = <int>[selectedFrameIdx];
    while (stack.isNotEmpty) {
      final sourceIdx = stack.removeLast();
      if (!selectedChainNodes.add(sourceIdx)) continue;
      for (final refIdx in _referenceCache.refsFor(sourceIdx)) {
        selectedChainEdges.add('$sourceIdx:$refIdx');
        if (!selectedChainNodes.contains(refIdx)) stack.add(refIdx);
      }
    }

    _selectedChainEdges = Set<String>.unmodifiable(selectedChainEdges);
    _selectedChainNodes = Set<int>.unmodifiable(selectedChainNodes);
  }

  int? _frameIndexAtChartPosition(Offset local, Size size) {
    if (widget.frames.isEmpty) return null;
    final visibleStart = widget.viewStart
        .floor()
        .clamp(0, widget.frames.length - 1)
        .toInt();
    final visibleEnd = (widget.viewEnd.ceil() + 1)
        .clamp(0, widget.frames.length)
        .toInt();
    if (visibleStart >= visibleEnd) return null;

    final axisH = size.height >= 96 ? analysisChartXAxisH : 0.0;
    final chartH = (size.height - axisH).clamp(1.0, double.infinity);
    final labelW = analysisChartLabelW;
    final usableW = (size.width - labelW).clamp(0.0, double.infinity);
    final plotRect = Rect.fromLTWH(labelW, 0, usableW, chartH);
    if (!plotRect.contains(local)) return null;

    var maxTid = 0;
    for (var i = visibleStart; i < visibleEnd; i++) {
      if (widget.frames[i].temporalId > maxTid) {
        maxTid = widget.frames[i].temporalId;
      }
    }
    final rowH = chartH / (maxTid + 1);
    final circleR = (rowH * 0.3).clamp(6.0, 20.0);
    final centerPad = circleR + analysisChartSelectedFramePadding;
    final centerW = (usableW - centerPad * 2).clamp(1.0, double.infinity);
    final span = widget.viewEnd - widget.viewStart;
    if (span <= 0) return null;

    final frameFrac = ((local.dx - labelW - centerPad) / centerW).clamp(
      0.0,
      1.0,
    );
    return (widget.viewStart + frameFrac * span)
        .round()
        .clamp(visibleStart, visibleEnd - 1)
        .toInt();
  }

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
                final box = context.findRenderObject() as RenderBox;
                final local = box.globalToLocal(signal.position);
                final axisH = box.size.height >= 96 ? analysisChartXAxisH : 0.0;
                final chartH = (box.size.height - axisH).clamp(
                  1.0,
                  double.infinity,
                );
                final inPlotContent =
                    local.dx >= analysisChartLabelW && local.dy < chartH;
                if (inPlotContent) {
                  panChartByWheel(
                    scrollDeltaY: signal.scrollDelta.dy,
                    viewStart: widget.viewStart,
                    viewEnd: widget.viewEnd,
                    total: widget.frames.length.toDouble(),
                    onPan: widget.onPan,
                  );
                } else {
                  widget.onZoom(signal.scrollDelta.dy);
                }
              }
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
                child: GestureDetector(
                  behavior: HitTestBehavior.opaque,
                  onTapUp: (details) {
                    final box = chartContext.findRenderObject() as RenderBox;
                    final frameIdx = _frameIndexAtChartPosition(
                      details.localPosition,
                      box.size,
                    );
                    widget.onFrameSelected(
                      frameIdx != null && widget.selectedFrameIdx == frameIdx
                          ? null
                          : frameIdx,
                    );
                  },
                  child: CustomPaint(
                    painter: _RefPyramidPainter(
                      frames: widget.frames,
                      referenceCache: _referenceCache,
                      currentIdx: widget.currentIdx,
                      selectedFrameIdx: widget.selectedFrameIdx,
                      selectedChainEdges: _selectedChainEdges,
                      selectedChainNodes: _selectedChainNodes,
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
        AnalysisChartScrollbar(
          total: widget.frames.length.toDouble(),
          viewStart: widget.viewStart,
          viewEnd: widget.viewEnd,
          onPan: widget.onPan,
        ),
      ],
    );
  }
}

class _FrameReferenceCache {
  final List<List<int>> refsByIndex;
  final List<List<int>> sourcesByRefIndex;

  const _FrameReferenceCache({
    required this.refsByIndex,
    required this.sourcesByRefIndex,
  });

  factory _FrameReferenceCache.build(
    List<FrameInfo> frames,
    Map<int, List<int>> pocToIndices,
  ) {
    int? nearestRefIdx(int refPoc, int sourceIdx) {
      final indices = pocToIndices[refPoc];
      if (indices == null || indices.isEmpty) return null;
      var best = indices[0];
      for (final idx in indices) {
        if ((idx - sourceIdx).abs() < (best - sourceIdx).abs()) best = idx;
      }
      return best;
    }

    final refsByIndex = List<List<int>>.generate(
      frames.length,
      (_) => const <int>[],
      growable: false,
    );
    final sourcesByRefIndex = List<List<int>>.generate(
      frames.length,
      (_) => <int>[],
      growable: false,
    );

    for (var i = 0; i < frames.length; i++) {
      final f = frames[i];
      if (f.numRefL0 == 0 && f.numRefL1 == 0) continue;
      final refs = <int>{};
      for (var j = 0; j < f.numRefL0 && j < f.refPocsL0.length; j++) {
        final ri = nearestRefIdx(f.refPocsL0[j], i);
        if (ri != null && ri >= 0 && ri < frames.length) refs.add(ri);
      }
      for (var j = 0; j < f.numRefL1 && j < f.refPocsL1.length; j++) {
        final ri = nearestRefIdx(f.refPocsL1[j], i);
        if (ri != null && ri >= 0 && ri < frames.length) refs.add(ri);
      }
      if (refs.isEmpty) continue;
      final refList = List<int>.unmodifiable(refs);
      refsByIndex[i] = refList;
      for (final refIdx in refList) {
        sourcesByRefIndex[refIdx].add(i);
      }
    }

    return _FrameReferenceCache(
      refsByIndex: refsByIndex,
      sourcesByRefIndex: [
        for (final sources in sourcesByRefIndex)
          List<int>.unmodifiable(sources),
      ],
    );
  }

  List<int> refsFor(int idx) {
    if (idx < 0 || idx >= refsByIndex.length) return const [];
    return refsByIndex[idx];
  }

  List<int> sourcesForRef(int idx) {
    if (idx < 0 || idx >= sourcesByRefIndex.length) return const [];
    return sourcesByRefIndex[idx];
  }
}

class _RefPyramidPainter extends CustomPainter {
  final List<FrameInfo> frames;
  final _FrameReferenceCache referenceCache;
  final int currentIdx;
  final int? selectedFrameIdx;
  final Set<String> selectedChainEdges;
  final Set<int> selectedChainNodes;
  final double viewStart;
  final double viewEnd;
  final bool ptsOrder;
  final Offset? hoverPosition;

  _RefPyramidPainter({
    required this.frames,
    required this.referenceCache,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.selectedChainEdges,
    required this.selectedChainNodes,
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
    final axisH = size.height >= 96 ? analysisChartXAxisH : 0.0;
    final chartH = (size.height - axisH).clamp(1.0, double.infinity);
    final numLevels = maxTid + 1;
    final rowH = chartH / numLevels;
    final labelW = analysisChartLabelW;
    final usableW = (size.width - labelW).clamp(0.0, double.infinity);
    final span = viewEnd - viewStart;
    final circleR = (rowH * 0.3).clamp(6.0, 20.0);
    final plotRect = Rect.fromLTWH(labelW, 0, usableW, chartH);
    final centerPad = circleR + analysisChartSelectedFramePadding;
    final centerW = (usableW - centerPad * 2).clamp(1.0, double.infinity);

    final positions = <int, Offset>{}; // globalIdx → position
    for (var i = visibleStart; i < visibleEnd; i++) {
      final frac = (i - viewStart) / span;
      final x = labelW + centerPad + frac * centerW;
      final y = chartH - (frames[i].temporalId + 0.5) * rowH;
      positions[i] = Offset(x, y);
    }
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
    // Reference relationships are cached by the parent and reused across
    // hover/current-frame repaints.

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
    final edgeCandidates = <int>{};
    for (var i = visibleStart; i < visibleEnd; i++) {
      edgeCandidates.add(i);
      edgeCandidates.addAll(referenceCache.sourcesForRef(i));
    }
    for (final i in edgeCandidates) {
      final refs = referenceCache.refsFor(i);
      if (refs.isEmpty) continue;
      final from = posFor(i);
      final sourceVisible = endpointVisible(i);

      for (final ri in refs) {
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
    _drawHoverOverlay(
      canvas,
      size,
      plotRect,
      positions,
      circleR,
      labelW,
      centerPad,
      centerW,
      span,
      chartH,
      visibleStart,
      visibleEnd,
    );
    canvas.restore();

    drawFrameXAxis(
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

  void _drawHoverOverlay(
    Canvas canvas,
    Size size,
    Rect plotRect,
    Map<int, Offset> positions,
    double circleR,
    double labelW,
    double centerPad,
    double centerW,
    double span,
    double chartH,
    int visibleStart,
    int visibleEnd,
  ) {
    final hover = hoverPosition;
    if (hover == null || !plotRect.contains(hover)) return;

    final frameFrac = ((hover.dx - labelW - centerPad) / centerW).clamp(
      0.0,
      1.0,
    );
    final frameIdx = (viewStart + frameFrac * span).round().clamp(
      visibleStart,
      visibleEnd - 1,
    );
    if (frameIdx < 0 || frameIdx >= frames.length) return;

    final f = frames[frameIdx];
    final pos =
        positions[frameIdx] ??
        Offset(
          labelW + centerPad + ((frameIdx - viewStart) / span) * centerW,
          hover.dy,
        );
    canvas.drawLine(
      Offset(pos.dx, 0),
      Offset(pos.dx, chartH),
      Paint()
        ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.3)
        ..strokeWidth = 1,
    );

    final sliceLabel = switch (f.sliceType) {
      2 => 'I',
      1 => 'P',
      _ => f.numRefL1 > 0 ? 'B' : 'B(uni)',
    };
    final lines = [
      '#$frameIdx  $sliceLabel  POC ${f.poc}',
      'Size: ${formatBytes(f.packetSize)}',
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
      referenceCache != old.referenceCache ||
      currentIdx != old.currentIdx ||
      selectedFrameIdx != old.selectedFrameIdx ||
      selectedChainEdges != old.selectedChainEdges ||
      selectedChainNodes != old.selectedChainNodes ||
      viewStart != old.viewStart ||
      viewEnd != old.viewEnd ||
      ptsOrder != old.ptsOrder ||
      hoverPosition != old.hoverPosition;
}

// ===========================================================================
