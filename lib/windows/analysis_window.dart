import 'dart:async';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import '../analysis/analysis_cache.dart';
import '../analysis/analysis_ffi.dart';
import '../analysis/nalu_types.dart';
import '../l10n/app_localizations.dart';

// ===========================================================================
// Analysis Window — secondary Flutter window for bitstream visualization
// ===========================================================================

class AnalysisApp extends StatelessWidget {
  final Color accentColor;
  final String hash;

  const AnalysisApp({super.key, required this.accentColor, required this.hash});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player - Analysis',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        brightness: Brightness.dark,
        colorSchemeSeed: accentColor,
        useMaterial3: true,
      ),
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: AnalysisPage(hash: hash),
    );
  }
}

// ===========================================================================

class AnalysisPage extends StatefulWidget {
  final String hash;
  const AnalysisPage({super.key, required this.hash});

  @override
  State<AnalysisPage> createState() => _AnalysisPageState();
}

class _AnalysisPageState extends State<AnalysisPage> {
  int _selectedTab = 0; // 0=ref pyramid, 1=frame trend
  bool _ptsOrder = true;
  int? _selectedNaluIdx;
  String _naluFilter = '';
  double _naluBrowserWidth = 300; // draggable splitter width for NALU browser
  int? _selectedFrameIdx;

  // Zoom / scroll state for top chart panel
  double _visibleFrameCount = 10;
  double _chartOffset = 0.0;

  void _chartZoom(double scrollDelta) {
    setState(() {
      final factor = scrollDelta > 0 ? 1.18 : 0.85;
      final oldCount = _visibleFrameCount;
      _visibleFrameCount = (_visibleFrameCount * factor)
          .clamp(3.0, _sortedFrames.length.toDouble());
      final center = _chartOffset + oldCount / 2;
      _chartOffset = center - _visibleFrameCount / 2;
      _clampChartOffset();
    });
  }

  void _chartPan(double newOffset) {
    setState(() {
      _chartOffset = newOffset;
      _clampChartOffset();
    });
  }

  void _clampChartOffset() {
    final max = (_sortedFrames.length - _visibleFrameCount).clamp(0.0, double.infinity);
    _chartOffset = _chartOffset.clamp(0.0, max);
  }

  List<FrameInfo> _frames = [];
  List<NaluInfo> _nalus = [];
  List<FrameInfo> _sortedFramesCache = [];
  NakiAnalysisSummary? _summary;
  Timer? _pollTimer;

  // Precomputed mappings rebuilt when data loads
  Map<int, List<int>> _pocToIndices = {};
  List<int> _frameToNalu = []; // frameIdx → naluIdx
  List<int?> _naluToFrame = []; // naluIdx → frameIdx (null if non-VCL)

  @override
  void initState() {
    super.initState();
    _loadData();
    _pollTimer = Timer.periodic(
      const Duration(milliseconds: 200),
      (_) => _poll(),
    );
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    super.dispose();
  }

  void _loadData() {
    final hash = widget.hash;
    final vbs2 = AnalysisCache.vbs2Path(hash);
    final vbi = AnalysisCache.vbiPath(hash);
    final vbt = AnalysisCache.vbtPath(hash);

    // Load via FFI (analysis files may already be loaded from main window)
    final s = AnalysisFfi.summary;
    if (s.loaded == 0) {
      AnalysisFfi.load(vbs2, vbi, vbt);
    }
    _readData();
  }

  void _readData() {
    final s = AnalysisFfi.summary;
    if (s.loaded == 0) return;
    _summary = s;
    _frames = AnalysisFfi.frames;
    _nalus = AnalysisFfi.nalus;
    _rebuildDerivedState();
    if (mounted) {
      setState(() {});
    }
  }

  void _rebuildDerivedState() {
    // POC → indices map
    _pocToIndices = <int, List<int>>{};
    for (var i = 0; i < _frames.length; i++) {
      (_pocToIndices[_frames[i].poc] ??= []).add(i);
    }

    // Frame ↔ NALU mappings (single pass)
    _frameToNalu = List<int>.filled(_frames.length, -1);
    _naluToFrame = List<int?>.filled(_nalus.length, null);
    var vclIdx = 0;
    for (var i = 0; i < _nalus.length; i++) {
      if ((_nalus[i].flags & 0x01) != 0) {
        if (vclIdx < _frames.length) {
          _frameToNalu[vclIdx] = i;
          _naluToFrame[i] = vclIdx;
        }
        vclIdx++;
      }
    }

    _rebuildSortedFramesCache();
  }

  void _poll() {
    final s = AnalysisFfi.summary;
    if (s.loaded == 0) return;
    if (_summary != null &&
        s.currentFrameIdx == _summary!.currentFrameIdx &&
        s.frameCount == _summary!.frameCount) {
      return;
    }
    _summary = s;
    if (mounted) {
      setState(() {});
    }
  }

  int? _frameToNaluIdx(int frameIdx) {
    if (frameIdx < 0 || frameIdx >= _frameToNalu.length) return null;
    final v = _frameToNalu[frameIdx];
    return v >= 0 ? v : null;
  }

  int? _naluToFrameIdx(int naluIdx) {
    if (naluIdx < 0 || naluIdx >= _naluToFrame.length) return null;
    return _naluToFrame[naluIdx];
  }

  List<FrameInfo> get _sortedFrames => _sortedFramesCache;

  void _rebuildSortedFramesCache() {
    _sortedFramesCache = List<FrameInfo>.from(_frames);
    if (!_ptsOrder) {
      _sortedFramesCache.sort((a, b) => a.dts.compareTo(b.dts));
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return Scaffold(
      body: Column(
        children: [
          // Top bar: order toggle + tab bar
          Container(
            height: 40,
            padding: const EdgeInsets.symmetric(horizontal: 8),
            decoration: BoxDecoration(
              border: Border(bottom: BorderSide(color: theme.dividerColor)),
            ),
            child: Row(
              children: [
                // Order toggle
                _OrderToggle(
                  ptsOrder: _ptsOrder,
                  onChanged: (v) => setState(() {
                  _ptsOrder = v;
                  _rebuildSortedFramesCache();
                }),
                  l: l,
                ),
                const Spacer(),
                // Tab bar
                _TabBar(
                  selectedTab: _selectedTab,
                  onTabChanged: (i) => setState(() => _selectedTab = i),
                  l: l,
                ),
              ],
            ),
          ),
          // Top panel: ref pyramid or frame trend (40%)
          Expanded(
            flex: 4,
            child: _selectedTab == 0
                ? _ReferencePyramidView(
                    frames: _sortedFrames,
                    currentIdx: _summary?.currentFrameIdx ?? -1,
                    selectedFrameIdx: _selectedFrameIdx,
                    pocToIndices: _pocToIndices,
                    onFrameSelected: (i) => setState(() {
                      _selectedFrameIdx = i;
                      _selectedNaluIdx = i != null ? _frameToNaluIdx(i) : null;
                    }),
                    viewStart: _chartOffset,
                    viewEnd: _chartOffset + _visibleFrameCount,
                    onZoom: _chartZoom,
                    onPan: _chartPan,
                    l: l,
                  )
                : _FrameTrendView(
                    frames: _sortedFrames,
                    currentIdx: _summary?.currentFrameIdx ?? -1,
                    viewStart: _chartOffset,
                    viewEnd: _chartOffset + _visibleFrameCount,
                    onZoom: _chartZoom,
                    onPan: _chartPan,
                    l: l,
                  ),
          ),
          const Divider(height: 1),
          // Bottom: NALU browser + detail (60%) — draggable splitter
          Expanded(
            flex: 6,
            child: LayoutBuilder(
              builder: (context, constraints) {
                final totalW = constraints.maxWidth;
                final maxBrowserW = totalW - 120; // leave at least 120px for detail
                final browserW = _naluBrowserWidth.clamp(120.0, maxBrowserW);
                return Stack(
                  children: [
                    Row(
                      children: [
                        // NALU browser
                        SizedBox(
                          width: browserW,
                          child: _NaluBrowserView(
                            nalus: _nalus,
                            selectedIdx: _selectedNaluIdx,
                            onSelected: (i) => setState(() {
                              _selectedNaluIdx = i;
                              _selectedFrameIdx = _naluToFrameIdx(i);
                            }),
                            filter: _naluFilter,
                            onFilterChanged: (v) => setState(() => _naluFilter = v),
                          ),
                        ),
                        // Visual-only divider line
                        Container(width: 1, color: theme.colorScheme.outlineVariant),
                        // NALU detail
                        Expanded(
                          child: _NaluDetailView(
                            nalu: _selectedNaluIdx != null && _selectedNaluIdx! < _nalus.length
                                ? _nalus[_selectedNaluIdx!]
                                : null,
                            frames: _frames,
                            l: l,
                          ),
                        ),
                      ],
                    ),
                    // Draggable splitter hit area
                    Positioned(
                      left: browserW - 4,
                      top: 0,
                      bottom: 0,
                      width: 9,
                      child: _ResizableVDivider(
                        position: browserW,
                        onPositionChanged: (v) => setState(() => _naluBrowserWidth = v),
                      ),
                    ),
                  ],
                );
              },
            ),
          ),
        ],
      ),
    );
  }
}

// ===========================================================================
// Top bar widgets
// ===========================================================================

class _OrderToggle extends StatelessWidget {
  final bool ptsOrder;
  final ValueChanged<bool> onChanged;
  final AppLocalizations l;
  const _OrderToggle({required this.ptsOrder, required this.onChanged, required this.l});

  @override
  Widget build(BuildContext context) {
    return SegmentedButton<bool>(
      segments: [
        ButtonSegment(value: true, label: Text(l.analysisPtsOrder)),
        ButtonSegment(value: false, label: Text(l.analysisDtsOrder)),
      ],
      selected: {ptsOrder},
      onSelectionChanged: (s) => onChanged(s.first),
      style: const ButtonStyle(
        visualDensity: VisualDensity.compact,
        textStyle: WidgetStatePropertyAll(TextStyle(fontSize: 12)),
      ),
    );
  }
}

class _TabBar extends StatelessWidget {
  final int selectedTab;
  final ValueChanged<int> onTabChanged;
  final AppLocalizations l;
  const _TabBar({required this.selectedTab, required this.onTabChanged, required this.l});

  @override
  Widget build(BuildContext context) {
    return SegmentedButton<int>(
      segments: [
        ButtonSegment(value: 0, label: Text(l.analysisRefPyramid)),
        ButtonSegment(value: 1, label: Text(l.analysisFrameTrend)),
      ],
      selected: {selectedTab},
      onSelectionChanged: (s) => onTabChanged(s.first),
      style: const ButtonStyle(
        visualDensity: VisualDensity.compact,
        textStyle: WidgetStatePropertyAll(TextStyle(fontSize: 12)),
      ),
    );
  }
}

// ===========================================================================
// Resizable vertical divider (drag to change left panel width)
// ===========================================================================

class _ResizableVDivider extends StatefulWidget {
  final double position;
  final ValueChanged<double> onPositionChanged;

  const _ResizableVDivider({required this.position, required this.onPositionChanged});

  @override
  State<_ResizableVDivider> createState() => _ResizableVDividerState();
}

class _ResizableVDividerState extends State<_ResizableVDivider> {
  bool _hovering = false;
  double _excess = 0.0;
  late double _effectivePos;

  @override
  void initState() {
    super.initState();
    _effectivePos = widget.position;
  }

  @override
  void didUpdateWidget(covariant _ResizableVDivider oldWidget) {
    super.didUpdateWidget(oldWidget);
    _effectivePos = widget.position;
  }

  void _onDragStart(_) {
    _excess = 0.0;
    _effectivePos = widget.position;
  }

  void _onDragUpdate(DragUpdateDetails details) {
    final desired = _effectivePos + _excess + details.delta.dx;
    final clamped = desired.clamp(120.0, double.maxFinite);
    _excess = desired - clamped;
    _effectivePos = clamped;
    widget.onPositionChanged(clamped);
  }

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onHorizontalDragStart: _onDragStart,
      onHorizontalDragUpdate: _onDragUpdate,
      child: MouseRegion(
        cursor: SystemMouseCursors.resizeColumn,
        onEnter: (_) => setState(() => _hovering = true),
        onExit: (_) => setState(() => _hovering = false),
        child: SizedBox.expand(
          child: Center(
            child: Container(
              width: _hovering ? 2 : 0,
              color: _hovering
                  ? Theme.of(context).colorScheme.primary
                  : Colors.transparent,
            ),
          ),
        ),
      ),
    );
  }
}

// ===========================================================================
// Reference Pyramid — circle nodes + reference arrows + level backgrounds
// Supports zoom (scroll wheel) and pan (scrollbar).
// ===========================================================================

class _ReferencePyramidView extends StatefulWidget {
  final List<FrameInfo> frames;
  final int currentIdx;
  final int? selectedFrameIdx;
  final Map<int, List<int>> pocToIndices;
  final ValueChanged<int?> onFrameSelected;
  final double viewStart;
  final double viewEnd;
  final ValueChanged<double> onZoom;
  final ValueChanged<double> onPan;
  final AppLocalizations l;
  const _ReferencePyramidView({
    required this.frames,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.pocToIndices,
    required this.onFrameSelected,
    required this.viewStart,
    required this.viewEnd,
    required this.onZoom,
    required this.onPan,
    required this.l,
  });

  @override
  State<_ReferencePyramidView> createState() => _ReferencePyramidViewState();
}

class _ReferencePyramidViewState extends State<_ReferencePyramidView> {
  _RefPyramidPainter? _lastPainter;

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
              child: CustomPaint(
                painter: _lastPainter = _RefPyramidPainter(
                  frames: widget.frames,
                  currentIdx: widget.currentIdx,
                  selectedFrameIdx: widget.selectedFrameIdx,
                  pocToIndices: widget.pocToIndices,
                  viewStart: widget.viewStart,
                  viewEnd: widget.viewEnd,
                ),
                size: Size.infinite,
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

  /// Populated during paint() — used by parent for hit-testing.
  List<(int, Rect)> frameRects = [];

  _RefPyramidPainter({
    required this.frames,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.pocToIndices,
    required this.viewStart,
    required this.viewEnd,
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
    final numLevels = maxTid + 1;
    final rowH = size.height / numLevels;
    final labelW = 36.0;
    final usableW = size.width - labelW;
    final span = viewEnd - viewStart;
    final circleR = (rowH * 0.3).clamp(6.0, 20.0);

    final positions = <int, Offset>{}; // globalIdx → position
    for (var i = visibleStart; i < visibleEnd; i++) {
      final frac = (i - viewStart) / span;
      final x = labelW + frac * usableW;
      final y = size.height - (frames[i].temporalId + 0.5) * rowH;
      positions[i] = Offset(x, y);
    }
    frameRects = [
      for (final e in positions.entries)
        (e.key, Rect.fromCircle(center: e.value, radius: circleR)),
    ];

    // --- Level backgrounds ---
    for (var tid = 0; tid <= maxTid; tid++) {
      final top = size.height - (tid + 1) * rowH;
      final alpha = 0.03 + (maxTid - tid) * 0.025;
      _bgPaint.color = const Color(0xFFFFFFFF).withValues(alpha: alpha);
      canvas.drawRect(Rect.fromLTWH(0, top, size.width, rowH), _bgPaint);
      final tp = TextPainter(
        text: TextSpan(
          text: 'L$tid',
          style: const TextStyle(
              color: Color(0xFFFFFFFF), fontSize: 10,
              fontWeight: FontWeight.w600, letterSpacing: 0.5),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      tp.paint(canvas, Offset(4, top + rowH / 2 - tp.height / 2));
    }
    // --- Current playback cursor ---
    if (currentIdx >= 0 && positions.containsKey(currentIdx)) {
      final cx = positions[currentIdx]!.dx;
      _cursorPaint.color = const Color(0xFFFFFFFF).withValues(alpha: 0.15);
      _cursorPaint.strokeWidth = 1;
      canvas.drawLine(Offset(cx, 0), Offset(cx, size.height), _cursorPaint);
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
      final x = labelW + frac * usableW;
      final y = size.height - (frames[idx].temporalId + 0.5) * rowH;
      return Offset(x, y);
    }

    // Collect selected frame's references for highlighting
    final Set<int> selectedRefs = {};
    if (selectedFrameIdx != null &&
        selectedFrameIdx! < frames.length &&
        positions.containsKey(selectedFrameIdx)) {
      final sf = frames[selectedFrameIdx!];
      for (var j = 0; j < sf.numRefL0 && j < sf.refPocsL0.length; j++) {
        final ri = nearestRefIdx(sf.refPocsL0[j], selectedFrameIdx!);
        if (ri != null && positions.containsKey(ri)) selectedRefs.add(ri);
      }
      for (var j = 0; j < sf.numRefL1 && j < sf.refPocsL1.length; j++) {
        final ri = nearestRefIdx(sf.refPocsL1[j], selectedFrameIdx!);
        if (ri != null && positions.containsKey(ri)) selectedRefs.add(ri);
      }
    }

    // Draw all visible arrows
    for (var i = visibleStart; i < visibleEnd; i++) {
      if (!positions.containsKey(i)) continue;
      final f = frames[i];
      if (f.numRefL0 == 0 && f.numRefL1 == 0) continue;
      final from = positions[i]!;

      final isSelLine = selectedFrameIdx != null &&
          (i == selectedFrameIdx || selectedRefs.contains(i));
      final lineW = isSelLine ? 2.5 : 1.0;
      final arrowAlpha = isSelLine ? 1.0 : (selectedFrameIdx != null ? 0.2 : 0.5);

      for (var j = 0; j < f.numRefL0 && j < f.refPocsL0.length; j++) {
        final ri = nearestRefIdx(f.refPocsL0[j], i);
        if (ri != null) {
          _drawArrow(canvas, from, posFor(ri),
              const Color(0xFF73D13D).withValues(alpha: arrowAlpha),
              lineW, circleR);
        }
      }
      for (var j = 0; j < f.numRefL1 && j < f.refPocsL1.length; j++) {
        final ri = nearestRefIdx(f.refPocsL1[j], i);
        if (ri != null) {
          _drawArrow(canvas, from, posFor(ri),
              const Color(0xFF40A9FF).withValues(alpha: arrowAlpha),
              lineW, circleR);
        }
      }
    }
    // --- Frame circles ---
    final related = <int>{};
    if (selectedFrameIdx != null) {
      related.add(selectedFrameIdx!);
      related.addAll(selectedRefs);
    }

    // Cache label TextPainters (only 3 variants: I, P, B)
    TextPainter? labelI, labelP, labelB;
    if (circleR >= 8) {
      labelI = TextPainter(
        text: const TextSpan(text: 'I',
            style: TextStyle(color: Color(0xFFFFFFFF), fontSize: 14,
                fontWeight: FontWeight.bold)),
        textDirection: TextDirection.ltr,
      )..layout();
      labelP = TextPainter(
        text: const TextSpan(text: 'P',
            style: TextStyle(color: Color(0xFFFFFFFF), fontSize: 14,
                fontWeight: FontWeight.bold)),
        textDirection: TextDirection.ltr,
      )..layout();
      labelB = TextPainter(
        text: const TextSpan(text: 'B',
            style: TextStyle(color: Color(0xFF0050B3), fontSize: 14,
                fontWeight: FontWeight.bold)),
        textDirection: TextDirection.ltr,
      )..layout();
    }

    for (final entry in positions.entries) {
      final i = entry.key;
      final pos = entry.value;
      final f = frames[i];
      final isSelected = i == selectedFrameIdx;
      final isRelated = related.contains(i);

      final Color fill, stroke;
      switch (f.sliceType) {
        case 2:
          fill = const Color(0xFFFF4D4F);
          stroke = const Color(0xFFCF1322);
        case 0:
          fill = const Color(0xFF52C41A);
          stroke = const Color(0xFF389E0D);
        default:
          fill = const Color(0xFFE6F7FF);
          stroke = const Color(0xFF1890FF);
      }

      final sw2 = isSelected ? 4.5 : (isRelated ? 3.5 : 2.5);
      final r = isSelected ? circleR + 2 : circleR;

      _fillPaint.color = fill;
      canvas.drawCircle(pos, r, _fillPaint);
      _strokePaint.color = stroke;
      _strokePaint.strokeWidth = sw2;
      canvas.drawCircle(pos, r, _strokePaint);

      if (circleR >= 8) {
        final tp = switch (f.sliceType) {
          2 => labelI!,
          0 => labelP!,
          _ => labelB!,
        };
        tp.paint(canvas, pos - Offset(tp.width / 2, tp.height / 2));
      }
    }
  }

  void _drawArrow(Canvas canvas, Offset from, Offset to, Color color,
      double strokeWidth, double circleR) {
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

  @override
  bool shouldRepaint(covariant _RefPyramidPainter old) =>
      frames.length != old.frames.length ||
      currentIdx != old.currentIdx ||
      selectedFrameIdx != old.selectedFrameIdx ||
      viewStart != old.viewStart ||
      viewEnd != old.viewEnd;
}

// ===========================================================================
// Frame Trend — zoomable / pannable bar chart
// ===========================================================================

class _FrameTrendView extends StatelessWidget {
  final List<FrameInfo> frames;
  final int currentIdx;
  final double viewStart;
  final double viewEnd;
  final ValueChanged<double> onZoom;
  final ValueChanged<double> onPan;
  final AppLocalizations l;
  const _FrameTrendView({
    required this.frames,
    required this.currentIdx,
    required this.viewStart,
    required this.viewEnd,
    required this.onZoom,
    required this.onPan,
    required this.l,
  });

  @override
  Widget build(BuildContext context) {
    if (frames.isEmpty) {
      return Center(child: Text(l.analysisNoFrameData));
    }
    return Column(
      children: [
        Expanded(
          child: Listener(
            onPointerSignal: (signal) {
              if (signal is PointerScrollEvent) {
                onZoom(signal.scrollDelta.dy);
              }
            },
            child: CustomPaint(
              painter: _FrameTrendPainter(
                frames: frames,
                currentIdx: currentIdx,
                viewStart: viewStart,
                viewEnd: viewEnd,
              ),
              size: Size.infinite,
            ),
          ),
        ),
        _ChartScrollbar(
          total: frames.length.toDouble(),
          viewStart: viewStart,
          viewEnd: viewEnd,
          onPan: onPan,
        ),
      ],
    );
  }
}

class _FrameTrendPainter extends CustomPainter {
  final List<FrameInfo> frames;
  final int currentIdx;
  final double viewStart;
  final double viewEnd;

  _FrameTrendPainter({
    required this.frames,
    required this.currentIdx,
    required this.viewStart,
    required this.viewEnd,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (frames.isEmpty) return;

    final visibleStart = viewStart.floor().clamp(0, frames.length - 1);
    final visibleEnd = (viewEnd.ceil() + 1).clamp(0, frames.length);
    if (visibleStart >= visibleEnd) return;

    final count = visibleEnd - visibleStart;
    final barW = (size.width / count).clamp(2.0, 40.0);
    final span = viewEnd - viewStart;
    final upperH = size.height * 0.6;
    final lowerH = size.height * 0.35;
    final gap = size.height * 0.05;

    int maxPacketSize = 1;
    for (var i = visibleStart; i < visibleEnd; i++) {
      if (frames[i].packetSize > maxPacketSize) maxPacketSize = frames[i].packetSize;
    }

    final barPaint = Paint()..style = PaintingStyle.fill;
    final qpPaint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5
      ..color = const Color(0xFFFFB74D);

    for (var i = visibleStart; i < visibleEnd; i++) {
      final f = frames[i];
      final frac = (i - viewStart) / span;
      final x = frac * size.width;
      final h = (f.packetSize / maxPacketSize) * upperH;

      barPaint.color = f.keyframe == 1
          ? const Color(0xFFFF5252)
          : const Color(0xFF42A5F5);
      canvas.drawRect(Rect.fromLTWH(x, upperH - h, barW - 1, h), barPaint);

      if (f.keyframe == 1) {
        canvas.drawRect(
          Rect.fromLTWH(x, upperH - h - 3, barW - 1, 2),
          Paint()..color = const Color(0xFFFFD54F),
        );
      }
    }

    // QP line
    final qpPath = Path();
    bool first = true;
    for (var i = visibleStart; i < visibleEnd; i++) {
      final f = frames[i];
      final frac = (i - viewStart) / span;
      final x = frac * size.width + barW / 2;
      final y = upperH + gap + (f.avgQp / 63.0) * lowerH;
      if (first) { qpPath.moveTo(x, y); first = false; }
      else { qpPath.lineTo(x, y); }
    }
    canvas.drawPath(qpPath, qpPaint);

    // Cursor
    if (currentIdx >= 0 && currentIdx < frames.length) {
      final frac = (currentIdx - viewStart) / span;
      final cx = frac * size.width;
      canvas.drawLine(
        Offset(cx, 0), Offset(cx, size.height),
        Paint()..color = const Color(0xFFFFFFFF).withValues(alpha: 0.7)
          ..strokeWidth = 1,
      );
    }
  }

  @override
  bool shouldRepaint(covariant _FrameTrendPainter old) =>
      frames.length != old.frames.length ||
      currentIdx != old.currentIdx ||
      viewStart != old.viewStart ||
      viewEnd != old.viewEnd;
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

    // Track
    canvas.drawRect(
      Rect.fromLTWH(0, size.height - 3, size.width, 3),
      Paint()..color = trackColor,
    );

    // Thumb
    final left = (viewStart / total) * size.width;
    final right = (viewEnd / total) * size.width;
    canvas.drawRRect(
      RRect.fromRectAndRadius(
        Rect.fromLTWH(left, 0, right - left, size.height),
        const Radius.circular(3),
      ),
      Paint()..color = thumbColor,
    );
  }

  @override
  bool shouldRepaint(covariant _ScrollbarPainter old) =>
      viewStart != old.viewStart || viewEnd != old.viewEnd || total != old.total;
}

// ===========================================================================
// NALU Browser — search bar + scrollable list
// ===========================================================================

class _NaluBrowserView extends StatefulWidget {
  final List<NaluInfo> nalus;
  final int? selectedIdx;
  final ValueChanged<int> onSelected;
  final String filter;
  final ValueChanged<String> onFilterChanged;

  const _NaluBrowserView({
    required this.nalus,
    required this.selectedIdx,
    required this.onSelected,
    required this.filter,
    required this.onFilterChanged,
  });

  @override
  State<_NaluBrowserView> createState() => _NaluBrowserViewState();
}

class _NaluBrowserViewState extends State<_NaluBrowserView> {
  final _scrollController = ScrollController();

  @override
  void dispose() {
    _scrollController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (widget.nalus.isEmpty) {
      return Center(child: Text(AppLocalizations.of(context)!.analysisNoNaluData));
    }
    final theme = Theme.of(context);
    final filter = widget.filter.toLowerCase();

    // Build filtered index map: visible[i] = original index
    final visible = <int>[];
    for (var i = 0; i < widget.nalus.length; i++) {
      if (filter.isEmpty) {
        visible.add(i);
      } else {
        final n = widget.nalus[i];
        final name = h266NaluTypeName(n.nalType).toLowerCase();
        final idStr = '#$i';
        if (name.contains(filter) || idStr.contains(filter) || '${n.nalType}'.contains(filter)) {
          visible.add(i);
        }
      }
    }

    return Column(
      children: [
        // Search bar — same height as list items (28px)
        Padding(
          padding: const EdgeInsets.all(4),
          child: TextField(
            onChanged: widget.onFilterChanged,
            style: theme.textTheme.bodySmall,
            decoration: InputDecoration(
              hintText: AppLocalizations.of(context)!.analysisFilterHint,
              hintStyle: theme.textTheme.bodySmall?.copyWith(
                color: theme.colorScheme.onSurfaceVariant.withValues(alpha: 0.5),
              ),
              prefixIcon: Icon(Icons.search, size: 14, color: theme.colorScheme.onSurfaceVariant),
              prefixIconConstraints: const BoxConstraints(minWidth: 20, minHeight: 0),
              isDense: true,
              contentPadding: const EdgeInsets.symmetric(horizontal: 4, vertical: 6),
              border: OutlineInputBorder(
                borderRadius: BorderRadius.circular(4),
                borderSide: BorderSide(color: theme.colorScheme.outlineVariant),
              ),
              enabledBorder: OutlineInputBorder(
                borderRadius: BorderRadius.circular(4),
                borderSide: BorderSide(color: theme.colorScheme.outlineVariant),
              ),
            ),
          ),
        ),
        // Result count
        if (filter.isNotEmpty)
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 4),
            child: Align(
              alignment: Alignment.centerLeft,
              child: Text(
                '${visible.length} / ${widget.nalus.length}',
                style: theme.textTheme.labelSmall?.copyWith(
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ),
            ),
          ),
        // List
        Expanded(
          child: ListView.builder(
            controller: _scrollController,
            itemCount: visible.length,
            itemExtent: 28,
            itemBuilder: (context, displayIndex) {
              final origIdx = visible[displayIndex];
              final n = widget.nalus[origIdx];
              final selected = origIdx == widget.selectedIdx;
              return InkWell(
                onTap: () => widget.onSelected(origIdx),
                child: Container(
                  color: selected
                      ? theme.colorScheme.primary.withValues(alpha: 0.15)
                      : null,
                  child: Row(
                    children: [
                      // Decorative color bar
                      Container(
                        width: 4,
                        height: 28,
                        color: naluTypeDecorColor(n.nalType),
                      ),
                      const SizedBox(width: 8),
                      // Index
                      SizedBox(
                        width: 40,
                        child: Text(
                          '#$origIdx',
                          style: theme.textTheme.bodySmall?.copyWith(
                            color: theme.colorScheme.onSurfaceVariant,
                            fontFeatures: [const FontFeature.tabularFigures()],
                          ),
                        ),
                      ),
                      const SizedBox(width: 6),
                      // Type number
                      SizedBox(
                        width: 24,
                        child: Text(
                          '${n.nalType}',
                          style: theme.textTheme.bodySmall?.copyWith(
                            color: theme.colorScheme.onSurfaceVariant,
                            fontFeatures: [const FontFeature.tabularFigures()],
                          ),
                        ),
                      ),
                      const SizedBox(width: 6),
                      // Type name
                      Expanded(
                        child: Text(
                          h266NaluTypeName(n.nalType),
                          style: theme.textTheme.bodySmall,
                        ),
                      ),
                    ],
                  ),
                ),
              );
            },
          ),
        ),
      ],
    );
  }
}

// ===========================================================================
// NALU Detail
// ===========================================================================

class _NaluDetailView extends StatelessWidget {
  final NaluInfo? nalu;
  final List<FrameInfo> frames;
  final AppLocalizations l;

  const _NaluDetailView({required this.nalu, required this.frames, required this.l});

  @override
  Widget build(BuildContext context) {
    if (nalu == null) {
      return Center(child: Text(l.analysisSelectNalu));
    }
    final n = nalu!;
    final theme = Theme.of(context);
    final ts = theme.textTheme.bodySmall!;

    final items = <_DetailRow>[
      _DetailRow(l.analysisType, '${h266NaluTypeName(n.nalType)} (${n.nalType})'),
      _DetailRow(l.analysisTemporalId, '${n.temporalId}'),
      _DetailRow(l.analysisLayerId, '${n.layerId}'),
      _DetailRow(l.analysisOffset, '${n.offset}'),
      _DetailRow(l.analysisSize, l.analysisBytes(n.size)),
      _DetailRow('VCL', '${(n.flags & 0x01) != 0}'),
      _DetailRow('Slice', '${(n.flags & 0x02) != 0}'),
      _DetailRow('Keyframe', '${(n.flags & 0x04) != 0}'),
    ];

    return Padding(
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(l.analysisNaluDetail, style: theme.textTheme.titleSmall),
          const SizedBox(height: 8),
          ...items.map((r) => Padding(
                padding: const EdgeInsets.symmetric(vertical: 2),
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    SizedBox(
                      width: 80,
                      child: Text(r.label,
                          style: ts.copyWith(
                              color: theme.colorScheme.onSurfaceVariant)),
                    ),
                    Expanded(child: Text(r.value, style: ts)),
                  ],
                ),
              )),
        ],
      ),
    );
  }
}

class _DetailRow {
  final String label;
  final String value;
  const _DetailRow(this.label, this.value);
}
