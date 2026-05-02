import 'dart:async';

import 'package:flutter/foundation.dart';

import '../../../analysis/analysis_cache.dart';
import '../../../analysis/analysis_ffi.dart';
import '../../../analysis/nalu_types.dart';
import 'analysis_page_state.dart';

class AnalysisPageController extends ChangeNotifier {
  static const int _frameReadChunk = 2048;
  static const int _naluReadChunk = 4096;
  static const int _frameBlockSize = 4096;
  static const int _naluBlockSize = 8192;
  static const int _frameBucketTargetCount = 1024;

  final String hash;
  final bool pollSummary;

  int _selectedTab = 0;
  bool _ptsOrder = true;
  int? _selectedNaluIdx;
  String _naluFilter = '';
  double _naluBrowserWidth = 300;
  int? _selectedFrameIdx;

  double _visibleFrameCount = 10;
  double _chartOffset = 0.0;
  double _frameSizeAxisZoom = 1.0;
  double _qpAxisZoom = 1.0;
  double _topPanelFraction = 0.40;

  List<FrameInfo> _frames = [];
  List<FrameBucket> _frameBuckets = [];
  int _frameBucketSize = 1;
  List<NaluInfo> _nalus = [];
  int _frameWindowStart = 0;
  int _naluWindowStart = 0;
  List<FrameInfo> _sortedFramesCache = [];
  List<int> _sortedFrameOriginalIndicesCache = [];
  Map<int, int> _frameToSortedPosition = {};
  Map<int, List<int>> _sortedPocToIndices = {};
  AnalysisSummary? _summary;
  AnalysisSession? _session;
  Timer? _pollTimer;

  AnalysisPageController({required this.hash, this.pollSummary = true});

  AnalysisPageViewModel get viewModel {
    return AnalysisPageViewModel(
      selectedTab: _selectedTab,
      ptsOrder: _ptsOrder,
      selectedNaluIdx: _selectedNaluIdx,
      naluFilter: _naluFilter,
      naluBrowserWidth: _naluBrowserWidth,
      selectedFrameIdx: _selectedFrameIdx,
      visibleFrameCount: _visibleFrameCount,
      chartOffset: _chartOffset,
      frameSizeAxisZoom: _frameSizeAxisZoom,
      qpAxisZoom: _qpAxisZoom,
      topPanelFraction: _topPanelFraction,
      frames: _frames,
      frameIndexBase: _frameWindowStart,
      totalFrameCount: _summary?.frameCount ?? _frames.length,
      frameBuckets: _frameBuckets,
      frameBucketSize: _frameBucketSize,
      nalus: _nalus,
      naluIndexBase: _naluWindowStart,
      totalNaluCount: _summary?.naluCount ?? _nalus.length,
      sortedFrames: _sortedFramesCache,
      sortedPocToIndices: _sortedPocToIndices,
      summary: _summary,
      codec: codec,
      selectedSortedFrameIdx: selectedSortedFrameIdx,
      currentSortedFrameIdx: currentSortedFrameIdx,
    );
  }

  AnalysisPageActions get actions {
    return AnalysisPageActions(
      onOrderChanged: setPtsOrder,
      onTabChanged: setTab,
      onChartZoom: chartZoom,
      onChartPan: chartPan,
      onAxisZoom: frameTrendAxisZoom,
      onChartFrameSelected: selectChartFrame,
      onNaluSelected: selectNalu,
      onNaluWindowRequested: requestNaluWindow,
      onChartWindowSetForTest: setChartWindowForTest,
      onNaluFilterChanged: setNaluFilter,
      onNaluBrowserWidthChanged: setNaluBrowserWidth,
      onTopPanelFractionChanged: setTopPanelFraction,
    );
  }

  List<FrameInfo> get frames => _frames;
  List<NaluInfo> get nalus => _nalus;
  int get frameIndexBase => _frameWindowStart;
  int get naluIndexBase => _naluWindowStart;
  AnalysisSummary? get summary => _summary;
  AnalysisCodec get codec => analysisCodecFromValue(_summary?.codec ?? 0);
  int? get selectedFrameIdx => _selectedFrameIdx;
  int? get selectedNaluIdx => _selectedNaluIdx;
  double get chartOffset => _chartOffset;
  double get visibleFrameCount => _visibleFrameCount;
  int get selectedTab => _selectedTab;
  bool get ptsOrder => _ptsOrder;
  bool get isLoaded =>
      (_summary?.loaded ?? 0) != 0 &&
      ((_summary?.frameCount ?? 0) > 0 || (_summary?.naluCount ?? 0) > 0);

  int? get selectedSortedFrameIdx => _selectedFrameIdx == null
      ? null
      : sortedPositionForFrameIdx(_selectedFrameIdx!);

  int get currentSortedFrameIdx {
    final idx = _summary?.currentFrameIdx ?? -1;
    return sortedPositionForFrameIdx(idx) ?? -1;
  }

  void start() {
    _loadData();
    if (pollSummary) {
      _pollTimer = Timer.periodic(
        const Duration(milliseconds: 200),
        (_) => poll(),
      );
    }
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    _session?.close();
    _session = null;
    super.dispose();
  }

  void loadDataForTest() => readData();

  void refreshExternalLayout() {
    notifyListeners();
  }

  void setTab(int tab) {
    if (_selectedTab == tab) return;
    _selectedTab = tab;
    _visibleFrameCount = _visibleFrameCount.clamp(
      _minVisibleFrameCount(),
      _maxVisibleFrameCount(),
    );
    _clampChartOffset();
    _ensureFrameWindowForChart();
    notifyListeners();
  }

  void setPtsOrder(bool ptsOrder) {
    if (_ptsOrder == ptsOrder) return;
    _ptsOrder = ptsOrder;
    _rebuildSortedFramesCache();
    _centerChartOnSelectedFrame();
    notifyListeners();
  }

  void selectNaluForTest(int naluIdx) => selectNalu(naluIdx);

  void chartZoom(double scrollDelta) {
    final maxCount = (_summary?.frameCount ?? _frames.length).toDouble();
    if (maxCount <= 0) return;
    final minCount = maxCount < 3.0 ? maxCount : 3.0;
    final factor = scrollDelta > 0 ? 1.18 : 0.85;
    final oldCount = _visibleFrameCount;
    _visibleFrameCount = (_visibleFrameCount * factor).clamp(
      minCount,
      _maxVisibleFrameCount(),
    );
    final center = _chartOffset + oldCount / 2;
    _chartOffset = center - _visibleFrameCount / 2;
    _clampChartOffset();
    _ensureFrameWindowForChart();
    notifyListeners();
  }

  void chartPan(double newOffset) {
    _chartOffset = newOffset;
    _clampChartOffset();
    _ensureFrameWindowForChart();
    notifyListeners();
  }

  void frameTrendAxisZoom(AnalysisFrameTrendAxis axis, double scrollDelta) {
    final factor = scrollDelta > 0 ? 0.85 : 1.18;
    switch (axis) {
      case AnalysisFrameTrendAxis.frameSize:
        _frameSizeAxisZoom = (_frameSizeAxisZoom * factor).clamp(0.25, 12.0);
      case AnalysisFrameTrendAxis.qp:
        _qpAxisZoom = (_qpAxisZoom * factor).clamp(0.5, 8.0);
    }
    notifyListeners();
  }

  void selectChartFrame(int? sortedIdx) {
    _selectFrame(
      sortedIdx != null ? _originalFrameIdxAtSortedPosition(sortedIdx) : null,
    );
    notifyListeners();
  }

  void selectNalu(int? naluIdx) {
    _selectedNaluIdx = naluIdx;
    if (naluIdx != null) {
      _ensureNaluWindowForIndex(naluIdx);
    }
    final frameIdx = naluIdx != null ? _naluToFrameIdx(naluIdx) : null;
    if (frameIdx != null) {
      _ensureFrameWindowForIndex(frameIdx);
    }
    _selectedFrameIdx = frameIdx;
    if (frameIdx != null) _centerChartOnFrame(frameIdx);
    notifyListeners();
  }

  void requestNaluWindow(int start, int count) {
    _loadNaluWindowForRange(start, count);
    _rebuildDerivedState();
    notifyListeners();
  }

  void setChartWindowForTest(double offset, double visibleFrameCount) {
    _visibleFrameCount = visibleFrameCount.clamp(
      _minVisibleFrameCount(),
      _maxVisibleFrameCount(),
    );
    _chartOffset = offset;
    _clampChartOffset();
    _ensureFrameWindowForChart();
    notifyListeners();
  }

  void setNaluFilter(String value) {
    if (_naluFilter == value) return;
    _naluFilter = value;
    notifyListeners();
  }

  void setNaluBrowserWidth(double value) {
    if ((_naluBrowserWidth - value).abs() < 0.01) return;
    _naluBrowserWidth = value;
    notifyListeners();
  }

  void setTopPanelFraction(double value) {
    if ((_topPanelFraction - value).abs() < 0.0001) return;
    _topPanelFraction = value;
    notifyListeners();
  }

  void readData() {
    final session = _session;
    if (session == null || !session.isOpen) return;
    final s = session.summary;
    if (s.loaded == 0) return;
    _summary = s;
    _visibleFrameCount = _visibleFrameCount.clamp(
      _minVisibleFrameCount(),
      _maxVisibleFrameCount(),
    );
    _loadFrameWindowForRange(_chartOffset.floor(), _visibleFrameCount.ceil());
    _loadNaluWindowForRange(_selectedNaluIdx ?? 0, 1);
    _rebuildDerivedState();
    notifyListeners();
  }

  void poll() {
    final session = _session;
    if (session == null || !session.isOpen) return;
    final s = session.summary;
    if (s.loaded == 0) return;
    if (_summary != null &&
        s.currentFrameIdx == _summary!.currentFrameIdx &&
        s.frameCount == _summary!.frameCount) {
      return;
    }
    _summary = s;
    notifyListeners();
  }

  int? sortedPositionForFrameIdx(int frameIdx) {
    return _frameToSortedPosition[frameIdx];
  }

  void _loadData() {
    final vbs2 = AnalysisCache.vbs2Path(hash);
    final vbi = AnalysisCache.vbiPath(hash);
    final vbt = AnalysisCache.vbtPath(hash);

    _session?.close();
    _frames = const [];
    _frameBuckets = const [];
    _frameBucketSize = 1;
    _nalus = const [];
    _frameWindowStart = 0;
    _naluWindowStart = 0;
    _sortedFramesCache = const [];
    _sortedFrameOriginalIndicesCache = const [];
    _frameToSortedPosition = {};
    _sortedPocToIndices = {};
    _session = AnalysisSession.open(vbs2, vbi, vbt);
    readData();
  }

  List<FrameInfo> _readFrameRangeInChunks(
    AnalysisSession session,
    int start,
    int count,
  ) {
    final result = <FrameInfo>[];
    var offset = start;
    var remaining = count;
    while (remaining > 0) {
      final chunk = remaining > _frameReadChunk ? _frameReadChunk : remaining;
      final frames = session.framesRange(offset, chunk);
      if (frames.isEmpty) break;
      result.addAll(frames);
      offset += frames.length;
      remaining -= frames.length;
      if (frames.length < chunk) break;
    }
    return result;
  }

  void _ensureFrameWindowForChart() {
    _loadFrameWindowForRange(_chartOffset.floor(), _visibleFrameCount.ceil());
    _rebuildDerivedState();
  }

  void _ensureFrameWindowForIndex(int frameIdx) {
    _loadFrameWindowForRange(frameIdx, 1);
    _rebuildDerivedState();
  }

  void _ensureNaluWindowForIndex(int naluIdx) {
    _loadNaluWindowForRange(naluIdx, 1);
    _rebuildDerivedState();
  }

  void _loadFrameWindowForRange(int start, int count) {
    final session = _session;
    final total = _summary?.frameCount ?? 0;
    if (session == null || !session.isOpen || total <= 0) {
      _frames = const [];
      _frameWindowStart = 0;
      return;
    }

    final range = _residentRange(
      start: start,
      count: count,
      total: total,
      blockSize: _frameBlockSize,
    );
    if (range == null) return;
    final (nextStart, nextCount) = range;
    if (_frameWindowStart == nextStart && _frames.length == nextCount) return;
    _frameWindowStart = nextStart;
    _frames = _readFrameRangeInChunks(session, nextStart, nextCount);
  }

  void _loadNaluWindowForRange(int start, int count) {
    final session = _session;
    final total = _summary?.naluCount ?? 0;
    if (session == null || !session.isOpen || total <= 0) {
      _nalus = const [];
      _naluWindowStart = 0;
      return;
    }

    final range = _residentRange(
      start: start,
      count: count,
      total: total,
      blockSize: _naluBlockSize,
    );
    if (range == null) return;
    final (nextStart, nextCount) = range;
    if (_naluWindowStart == nextStart && _nalus.length == nextCount) return;
    _naluWindowStart = nextStart;
    _nalus = _readNaluRangeInChunks(session, nextStart, nextCount);
  }

  (int start, int count)? _residentRange({
    required int start,
    required int count,
    required int total,
    required int blockSize,
  }) {
    if (total <= 0) return null;
    final safeStart = start.clamp(0, total - 1).toInt();
    final safeEnd = (safeStart + count).clamp(safeStart + 1, total).toInt();
    final blockStart = (safeStart ~/ blockSize - 1).clamp(0, total).toInt();
    final blockEnd = ((safeEnd + blockSize - 1) ~/ blockSize + 1)
        .clamp(0, (total + blockSize - 1) ~/ blockSize)
        .toInt();
    final residentStart = (blockStart * blockSize).clamp(0, total).toInt();
    final residentEnd = (blockEnd * blockSize)
        .clamp(residentStart, total)
        .toInt();
    return (residentStart, residentEnd - residentStart);
  }

  List<NaluInfo> _readNaluRangeInChunks(
    AnalysisSession session,
    int start,
    int count,
  ) {
    final result = <NaluInfo>[];
    var offset = start;
    var remaining = count;
    while (remaining > 0) {
      final chunk = remaining > _naluReadChunk ? _naluReadChunk : remaining;
      final nalus = session.nalusRange(offset, chunk);
      if (nalus.isEmpty) break;
      result.addAll(nalus);
      offset += nalus.length;
      remaining -= nalus.length;
      if (nalus.length < chunk) break;
    }
    return result;
  }

  void _rebuildDerivedState() {
    _rebuildSortedFramesCache();
    _refreshFrameBuckets();
  }

  void _refreshFrameBuckets() {
    final session = _session;
    final total = _summary?.frameCount ?? 0;
    final visibleCount = _visibleFrameCount.ceil();
    final bucketSize = _resolveFrameBucketSize();
    if (_selectedTab != 1 ||
        _ptsOrder ||
        session == null ||
        !session.isOpen ||
        total <= 0 ||
        visibleCount <= 0 ||
        bucketSize <= 1) {
      _frameBuckets = const [];
      _frameBucketSize = 1;
      return;
    }

    final start = _chartOffset.floor().clamp(0, total - 1).toInt();
    final bucketCount = ((visibleCount + bucketSize - 1) ~/ bucketSize + 2)
        .clamp(1, total)
        .toInt();
    _frameBuckets = session.frameBuckets(
      start: start,
      bucketSize: bucketSize,
      maxCount: bucketCount,
    );
    _frameBucketSize = bucketSize;
  }

  void _rebuildSortedFramesCache() {
    final order = List<int>.generate(_frames.length, (i) => i);
    if (_ptsOrder) {
      order.sort((a, b) => _frames[a].pts.compareTo(_frames[b].pts));
    }
    _sortedFrameOriginalIndicesCache = [
      for (final idx in order) _frameWindowStart + idx,
    ];
    _sortedFramesCache = [for (final idx in order) _frames[idx]];
    _frameToSortedPosition = <int, int>{};
    _sortedPocToIndices = <int, List<int>>{};
    for (var sortedIdx = 0; sortedIdx < order.length; sortedIdx++) {
      final originalIdx = _frameWindowStart + order[sortedIdx];
      final displayIdx = _frameWindowStart + sortedIdx;
      _frameToSortedPosition[originalIdx] = displayIdx;
      final frame = _frames[order[sortedIdx]];
      (_sortedPocToIndices[frame.poc] ??= []).add(displayIdx);
    }
    _clampChartOffset();
  }

  int? _frameToNaluIdx(int frameIdx) {
    final session = _session;
    if (session == null || !session.isOpen) return null;
    final v = session.frameToNalu(frameIdx);
    return v >= 0 ? v : null;
  }

  int? _naluToFrameIdx(int naluIdx) {
    final session = _session;
    if (session == null || !session.isOpen) return null;
    final v = session.naluToFrame(naluIdx);
    return v >= 0 ? v : null;
  }

  int? _originalFrameIdxAtSortedPosition(int sortedIdx) {
    final localIdx = sortedIdx - _frameWindowStart;
    if (localIdx < 0 || localIdx >= _sortedFrameOriginalIndicesCache.length) {
      return null;
    }
    return _sortedFrameOriginalIndicesCache[localIdx];
  }

  void _selectFrame(int? frameIdx, {bool centerChart = false}) {
    _selectedFrameIdx = frameIdx;
    _selectedNaluIdx = frameIdx != null ? _frameToNaluIdx(frameIdx) : null;
    if (centerChart && frameIdx != null) _centerChartOnFrame(frameIdx);
  }

  void _centerChartOnSelectedFrame() {
    final idx = _selectedFrameIdx;
    if (idx != null) _centerChartOnFrame(idx);
  }

  void _centerChartOnFrame(int frameIdx) {
    final sortedIdx = sortedPositionForFrameIdx(frameIdx);
    if (sortedIdx == null) return;
    _chartOffset = sortedIdx - _visibleFrameCount / 2 + 0.5;
    _clampChartOffset();
  }

  void _clampChartOffset() {
    final total = (_summary?.frameCount ?? _sortedFramesCache.length)
        .toDouble();
    final max = (total - _visibleFrameCount).clamp(0.0, double.infinity);
    _chartOffset = _chartOffset.clamp(0.0, max);
  }

  double _minVisibleFrameCount() {
    final total = (_summary?.frameCount ?? _frames.length).toDouble();
    if (total <= 0) return 0.0;
    return total < 3.0 ? total : 3.0;
  }

  double _maxVisibleFrameCount() {
    final total = (_summary?.frameCount ?? _frames.length).toDouble();
    if (total <= 0) return 0.0;
    return total < _frameBlockSize ? total : _frameBlockSize.toDouble();
  }

  int _resolveFrameBucketSize() {
    if (_ptsOrder) return 1;
    final visibleCount = _visibleFrameCount.ceil();
    if (visibleCount <= _frameBucketTargetCount) return 1;
    return ((visibleCount + _frameBucketTargetCount - 1) ~/
            _frameBucketTargetCount)
        .clamp(1, _frameBlockSize)
        .toInt();
  }
}
