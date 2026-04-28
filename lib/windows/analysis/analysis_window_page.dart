import 'dart:async';

import 'package:flutter/material.dart';

import '../../analysis/analysis_cache.dart';
import '../../analysis/analysis_ffi.dart';
import '../../analysis/nalu_types.dart';
import '../../l10n/app_localizations.dart';
import 'analysis_split_layout_controller.dart';
import 'analysis_test_host.dart';
import 'analysis_window_charts.dart';
import 'analysis_window_controls.dart';
import 'analysis_window_nalu.dart';
import 'analysis_window_style.dart';
import 'analysis_window_test_runner.dart';

class AnalysisPage extends StatefulWidget {
  final String hash;
  final String? testScriptPath;
  final bool pollSummary;
  final AnalysisSplitLayoutController? splitLayoutController;

  const AnalysisPage({
    super.key,
    required this.hash,
    this.testScriptPath,
    this.pollSummary = true,
    this.splitLayoutController,
  });

  @override
  State<AnalysisPage> createState() => AnalysisPageState();
}

class AnalysisPageState extends State<AnalysisPage>
    implements AnalysisTestHost {
  int _selectedTab = 0; // 0=ref pyramid, 1=frame trend
  bool _ptsOrder = true;
  int? _selectedNaluIdx;
  String _naluFilter = '';
  double _naluBrowserWidth = 300; // draggable splitter width for NALU browser
  int? _selectedFrameIdx;

  // Zoom / scroll state for top chart panel
  double _visibleFrameCount = 10;
  double _chartOffset = 0.0;
  double _frameSizeAxisZoom = 1.0;
  double _qpAxisZoom = 1.0;
  double _topPanelFraction = 0.40;

  void _chartZoom(double scrollDelta) {
    setState(() {
      final factor = scrollDelta > 0 ? 1.18 : 0.85;
      final oldCount = _visibleFrameCount;
      _visibleFrameCount = (_visibleFrameCount * factor).clamp(
        3.0,
        _sortedFrames.length.toDouble(),
      );
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

  void _frameTrendAxisZoom(AnalysisFrameTrendAxis axis, double scrollDelta) {
    setState(() {
      final factor = scrollDelta > 0 ? 0.85 : 1.18;
      switch (axis) {
        case AnalysisFrameTrendAxis.frameSize:
          _frameSizeAxisZoom = (_frameSizeAxisZoom * factor).clamp(0.25, 12.0);
        case AnalysisFrameTrendAxis.qp:
          _qpAxisZoom = (_qpAxisZoom * factor).clamp(0.5, 8.0);
      }
    });
  }

  void _clampChartOffset() {
    final max = (_sortedFrames.length - _visibleFrameCount).clamp(
      0.0,
      double.infinity,
    );
    _chartOffset = _chartOffset.clamp(0.0, max);
  }

  List<FrameInfo> _frames = [];
  List<NaluInfo> _nalus = [];
  List<FrameInfo> _sortedFramesCache = [];
  List<int> _sortedFrameOriginalIndicesCache = [];
  List<int> _frameToSortedPosition = [];
  Map<int, List<int>> _sortedPocToIndices = {};
  NakiAnalysisSummary? _summary;
  Timer? _pollTimer;
  bool _testStarted = false;

  // Precomputed mappings rebuilt when data loads
  List<int> _frameToNalu = []; // frameIdx → naluIdx
  List<int?> _naluToFrame = []; // naluIdx → frameIdx (null if non-VCL)

  @override
  void initState() {
    super.initState();
    widget.splitLayoutController?.addListener(_onSplitLayoutChanged);
    _loadData();
    if (widget.pollSummary) {
      _pollTimer = Timer.periodic(
        const Duration(milliseconds: 200),
        (_) => _poll(),
      );
    }
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final scriptPath = widget.testScriptPath;
      if (scriptPath != null && !_testStarted) {
        _testStarted = true;
        unawaited(runAnalysisTestScript(scriptPath));
      }
    });
  }

  @override
  void didUpdateWidget(covariant AnalysisPage oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.splitLayoutController != widget.splitLayoutController) {
      oldWidget.splitLayoutController?.removeListener(_onSplitLayoutChanged);
      widget.splitLayoutController?.addListener(_onSplitLayoutChanged);
    }
  }

  @override
  void dispose() {
    widget.splitLayoutController?.removeListener(_onSplitLayoutChanged);
    _pollTimer?.cancel();
    super.dispose();
  }

  void _onSplitLayoutChanged() {
    if (mounted) setState(() {});
  }

  @override
  void updateAnalysisTestState(VoidCallback update) {
    if (mounted) {
      setState(update);
    } else {
      update();
    }
  }

  void _loadData() {
    final hash = widget.hash;
    final vbs2 = AnalysisCache.vbs2Path(hash);
    final vbi = AnalysisCache.vbiPath(hash);
    final vbt = AnalysisCache.vbtPath(hash);

    AnalysisFfi.load(vbs2, vbi, vbt);
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

  @override
  List<FrameInfo> get analysisFrames => _frames;

  @override
  List<NaluInfo> get analysisNalus => _nalus;

  @override
  NakiAnalysisSummary? get analysisSummary => _summary;

  @override
  AnalysisCodec get analysisCodec => _codec;

  @override
  int? get selectedAnalysisFrameIdx => _selectedFrameIdx;

  @override
  int? get selectedAnalysisNaluIdx => _selectedNaluIdx;

  @override
  double get analysisChartOffset => _chartOffset;

  @override
  double get analysisVisibleFrameCount => _visibleFrameCount;

  @override
  int get analysisSelectedTab => _selectedTab;

  @override
  bool get analysisPtsOrder => _ptsOrder;

  @override
  void readAnalysisDataForTest() => _readData();

  @override
  bool get isAnalysisLoaded =>
      (_summary?.loaded ?? 0) != 0 && (_frames.isNotEmpty || _nalus.isNotEmpty);

  @override
  void setAnalysisTabForTest(int tab) {
    _selectedTab = tab;
  }

  @override
  void setAnalysisOrderForTest(bool ptsOrder) {
    _ptsOrder = ptsOrder;
    _rebuildSortedFramesCache();
  }

  @override
  void selectAnalysisNaluForTest(int naluIdx) {
    _selectNalu(naluIdx);
  }

  int? get _selectedSortedFrameIdx => _selectedFrameIdx == null
      ? null
      : _sortedPositionForFrameIdx(_selectedFrameIdx!);
  int get _currentSortedFrameIdx {
    final idx = _summary?.currentFrameIdx ?? -1;
    return _sortedPositionForFrameIdx(idx) ?? -1;
  }

  AnalysisCodec get _codec => analysisCodecFromValue(_summary?.codec ?? 0);

  void _rebuildSortedFramesCache() {
    final order = List<int>.generate(_frames.length, (i) => i);
    if (_ptsOrder) {
      order.sort((a, b) => _frames[a].pts.compareTo(_frames[b].pts));
    }
    // else: keep original order (= DTS / decode order from C++)
    _sortedFrameOriginalIndicesCache = order;
    _sortedFramesCache = [for (final idx in order) _frames[idx]];
    _frameToSortedPosition = List<int>.filled(_frames.length, -1);
    _sortedPocToIndices = <int, List<int>>{};
    for (var sortedIdx = 0; sortedIdx < order.length; sortedIdx++) {
      final originalIdx = order[sortedIdx];
      _frameToSortedPosition[originalIdx] = sortedIdx;
      (_sortedPocToIndices[_frames[originalIdx].poc] ??= []).add(sortedIdx);
    }
    _clampChartOffset();
  }

  @override
  int? sortedPositionForFrameIdx(int frameIdx) =>
      _sortedPositionForFrameIdx(frameIdx);

  int? _sortedPositionForFrameIdx(int frameIdx) {
    if (frameIdx < 0 || frameIdx >= _frameToSortedPosition.length) return null;
    final sortedIdx = _frameToSortedPosition[frameIdx];
    return sortedIdx >= 0 ? sortedIdx : null;
  }

  int? _originalFrameIdxAtSortedPosition(int sortedIdx) {
    if (sortedIdx < 0 || sortedIdx >= _sortedFrameOriginalIndicesCache.length) {
      return null;
    }
    return _sortedFrameOriginalIndicesCache[sortedIdx];
  }

  void _centerChartOnFrame(int frameIdx) {
    final sortedIdx = _sortedPositionForFrameIdx(frameIdx);
    if (sortedIdx == null) return;
    _chartOffset = sortedIdx - _visibleFrameCount / 2 + 0.5;
    _clampChartOffset();
  }

  void _centerChartOnSelectedFrame() {
    final idx = _selectedFrameIdx;
    if (idx != null) _centerChartOnFrame(idx);
  }

  void _selectFrame(int? frameIdx, {bool centerChart = false}) {
    _selectedFrameIdx = frameIdx;
    _selectedNaluIdx = frameIdx != null ? _frameToNaluIdx(frameIdx) : null;
    if (centerChart && frameIdx != null) _centerChartOnFrame(frameIdx);
  }

  void _selectChartFrame(int? sortedIdx) {
    _selectFrame(
      sortedIdx != null ? _originalFrameIdxAtSortedPosition(sortedIdx) : null,
    );
  }

  void _selectNalu(int? naluIdx) {
    _selectedNaluIdx = naluIdx;
    final frameIdx = naluIdx != null ? _naluToFrameIdx(naluIdx) : null;
    _selectedFrameIdx = frameIdx;
    if (frameIdx != null) _centerChartOnFrame(frameIdx);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    final topChart = _selectedTab == 0
        ? AnalysisReferencePyramidView(
            frames: _sortedFrames,
            currentIdx: _currentSortedFrameIdx,
            selectedFrameIdx: _selectedSortedFrameIdx,
            pocToIndices: _sortedPocToIndices,
            onFrameSelected: (i) => setState(() => _selectChartFrame(i)),
            viewStart: _chartOffset,
            viewEnd: _chartOffset + _visibleFrameCount,
            ptsOrder: _ptsOrder,
            onZoom: _chartZoom,
            onPan: _chartPan,
            l: l,
          )
        : AnalysisFrameTrendView(
            frames: _sortedFrames,
            currentIdx: _currentSortedFrameIdx,
            selectedFrameIdx: _selectedSortedFrameIdx,
            viewStart: _chartOffset,
            viewEnd: _chartOffset + _visibleFrameCount,
            frameSizeAxisZoom: _frameSizeAxisZoom,
            qpAxisZoom: _qpAxisZoom,
            ptsOrder: _ptsOrder,
            onZoom: _chartZoom,
            onAxisZoom: _frameTrendAxisZoom,
            onPan: _chartPan,
            onFrameSelected: (i) => setState(() => _selectChartFrame(i)),
            l: l,
          );
    final bottomPanel = LayoutBuilder(
      builder: (context, constraints) {
        final totalW = constraints.maxWidth;
        final maxBrowserW = totalW - 120; // leave room for detail
        final layoutController = widget.splitLayoutController;
        final requestedBrowserW = layoutController != null
            ? totalW * layoutController.naluBrowserFraction
            : _naluBrowserWidth;
        final browserW = requestedBrowserW.clamp(120.0, maxBrowserW);
        return Stack(
          children: [
            Row(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                SizedBox(
                  width: browserW,
                  child: AnalysisNaluBrowserView(
                    nalus: _nalus,
                    codec: _codec,
                    selectedIdx: _selectedNaluIdx,
                    onSelected: (i) => setState(() => _selectNalu(i)),
                    filter: _naluFilter,
                    onFilterChanged: (v) => setState(() => _naluFilter = v),
                  ),
                ),
                Container(width: 1, color: theme.colorScheme.outlineVariant),
                Expanded(
                  child: AnalysisNaluDetailView(
                    nalu:
                        _selectedNaluIdx != null &&
                            _selectedNaluIdx! < _nalus.length
                        ? _nalus[_selectedNaluIdx!]
                        : null,
                    frameIdx: _selectedFrameIdx,
                    frames: _frames,
                    codec: _codec,
                    l: l,
                  ),
                ),
              ],
            ),
            Positioned(
              left: browserW - 4,
              top: 0,
              bottom: 0,
              width: 9,
              child: AnalysisResizableVDivider(
                position: browserW,
                onPositionChanged: (v) {
                  final clamped = v.clamp(120.0, maxBrowserW);
                  if (layoutController != null) {
                    layoutController.setNaluBrowserFraction(
                      totalW <= 0 ? 0.0 : clamped / totalW,
                    );
                  } else {
                    setState(() => _naluBrowserWidth = clamped);
                  }
                },
              ),
            ),
          ],
        );
      },
    );
    return Scaffold(
      body: Column(
        children: [
          // Top bar: order toggle + tab bar
          Container(
            height: analysisHeaderHeight,
            padding: analysisHeaderPadding,
            decoration: BoxDecoration(
              border: Border(bottom: BorderSide(color: theme.dividerColor)),
            ),
            child: Row(
              children: [
                // Order toggle
                SizedBox(
                  height: analysisHeaderControlHeight,
                  child: AnalysisOrderToggle(
                    ptsOrder: _ptsOrder,
                    onChanged: (v) => setState(() {
                      _ptsOrder = v;
                      _rebuildSortedFramesCache();
                      _centerChartOnSelectedFrame();
                    }),
                    l: l,
                  ),
                ),
                const Spacer(),
                // Tab bar
                SizedBox(
                  height: analysisHeaderControlHeight,
                  child: AnalysisViewTabBar(
                    selectedTab: _selectedTab,
                    onTabChanged: (i) => setState(() => _selectedTab = i),
                    l: l,
                  ),
                ),
              ],
            ),
          ),
          Expanded(
            // Top chart and bottom NALU/detail area share a draggable divider.
            child: LayoutBuilder(
              builder: (context, constraints) {
                const dividerHitH = 10.0;
                final available = constraints.maxHeight.clamp(
                  0.0,
                  double.infinity,
                );
                final compact = available < 280;
                final minTop = compact ? available * 0.28 : 120.0;
                final minBottom = compact ? available * 0.28 : 170.0;
                final maxTop = (available - minBottom).clamp(minTop, available);
                final layoutController = widget.splitLayoutController;
                final topPanelFraction =
                    layoutController?.topPanelFraction ?? _topPanelFraction;
                final topH = (available * topPanelFraction).clamp(
                  minTop,
                  maxTop,
                );
                final bottomH = available - topH;
                final dividerTop = (topH - dividerHitH / 2).clamp(
                  0.0,
                  (available - dividerHitH).clamp(0.0, double.infinity),
                );
                return Stack(
                  children: [
                    Positioned(
                      left: 0,
                      right: 0,
                      top: 0,
                      height: topH,
                      child: topChart,
                    ),
                    Positioned(
                      left: 0,
                      right: 0,
                      top: topH,
                      height: bottomH,
                      child: bottomPanel,
                    ),
                    Positioned(
                      left: 0,
                      right: 0,
                      top: dividerTop,
                      height: dividerHitH,
                      child: AnalysisResizableHDivider(
                        position: topH,
                        minPosition: minTop,
                        maxPosition: maxTop,
                        onPositionChanged: (nextTop) {
                          if (available <= 0) return;
                          final nextFraction = nextTop / available;
                          if (layoutController != null) {
                            layoutController.setTopPanelFraction(nextFraction);
                          } else {
                            setState(() {
                              _topPanelFraction = nextFraction;
                            });
                          }
                        },
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
