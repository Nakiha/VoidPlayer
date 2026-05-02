import 'dart:async';

import 'package:flutter/foundation.dart';

import '../../../analysis/analysis_cache.dart';
import '../../../analysis/analysis_ffi.dart';
import '../../../analysis/nalu_types.dart';
import 'analysis_page_state.dart';

class AnalysisPageController extends ChangeNotifier {
  static const int _frameReadChunk = 2048;
  static const int _naluReadChunk = 4096;
  static const int _maxResidentFrames = 20000;
  static const int _maxResidentNalus = 50000;

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
  List<NaluInfo> _nalus = [];
  List<FrameInfo> _sortedFramesCache = [];
  List<int> _sortedFrameOriginalIndicesCache = [];
  List<int> _frameToSortedPosition = [];
  Map<int, List<int>> _sortedPocToIndices = {};
  AnalysisSummary? _summary;
  AnalysisSession? _session;
  Timer? _pollTimer;

  List<int> _frameToNalu = [];
  List<int?> _naluToFrame = [];

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
      nalus: _nalus,
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
      onNaluFilterChanged: setNaluFilter,
      onNaluBrowserWidthChanged: setNaluBrowserWidth,
      onTopPanelFractionChanged: setTopPanelFraction,
    );
  }

  List<FrameInfo> get frames => _frames;
  List<NaluInfo> get nalus => _nalus;
  AnalysisSummary? get summary => _summary;
  AnalysisCodec get codec => analysisCodecFromValue(_summary?.codec ?? 0);
  int? get selectedFrameIdx => _selectedFrameIdx;
  int? get selectedNaluIdx => _selectedNaluIdx;
  double get chartOffset => _chartOffset;
  double get visibleFrameCount => _visibleFrameCount;
  int get selectedTab => _selectedTab;
  bool get ptsOrder => _ptsOrder;
  bool get isLoaded =>
      (_summary?.loaded ?? 0) != 0 && (_frames.isNotEmpty || _nalus.isNotEmpty);

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
    final maxCount = _sortedFramesCache.length.toDouble();
    if (maxCount <= 0) return;
    final minCount = maxCount < 3.0 ? maxCount : 3.0;
    final factor = scrollDelta > 0 ? 1.18 : 0.85;
    final oldCount = _visibleFrameCount;
    _visibleFrameCount = (_visibleFrameCount * factor).clamp(
      minCount,
      maxCount,
    );
    final center = _chartOffset + oldCount / 2;
    _chartOffset = center - _visibleFrameCount / 2;
    _clampChartOffset();
    notifyListeners();
  }

  void chartPan(double newOffset) {
    _chartOffset = newOffset;
    _clampChartOffset();
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
    final frameIdx = naluIdx != null ? _naluToFrameIdx(naluIdx) : null;
    _selectedFrameIdx = frameIdx;
    if (frameIdx != null) _centerChartOnFrame(frameIdx);
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
    _frames = _readFrames(session, s.frameCount);
    _nalus = _readNalus(session, s.naluCount);
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
    if (frameIdx < 0 || frameIdx >= _frameToSortedPosition.length) return null;
    final sortedIdx = _frameToSortedPosition[frameIdx];
    return sortedIdx >= 0 ? sortedIdx : null;
  }

  void _loadData() {
    final vbs2 = AnalysisCache.vbs2Path(hash);
    final vbi = AnalysisCache.vbiPath(hash);
    final vbt = AnalysisCache.vbtPath(hash);

    _session?.close();
    _session = AnalysisSession.open(vbs2, vbi, vbt);
    readData();
  }

  List<FrameInfo> _readFrames(AnalysisSession session, int total) {
    final count = total.clamp(0, _maxResidentFrames).toInt();
    if (count == 0) return const [];
    return _readFrameRangeInChunks(session, 0, count);
  }

  List<NaluInfo> _readNalus(AnalysisSession session, int total) {
    final count = total.clamp(0, _maxResidentNalus).toInt();
    if (count == 0) return const [];
    return _readNaluRangeInChunks(session, 0, count);
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

  void _rebuildSortedFramesCache() {
    final order = List<int>.generate(_frames.length, (i) => i);
    if (_ptsOrder) {
      order.sort((a, b) => _frames[a].pts.compareTo(_frames[b].pts));
    }
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

  int? _frameToNaluIdx(int frameIdx) {
    if (frameIdx < 0 || frameIdx >= _frameToNalu.length) return null;
    final v = _frameToNalu[frameIdx];
    return v >= 0 ? v : null;
  }

  int? _naluToFrameIdx(int naluIdx) {
    if (naluIdx < 0 || naluIdx >= _naluToFrame.length) return null;
    return _naluToFrame[naluIdx];
  }

  int? _originalFrameIdxAtSortedPosition(int sortedIdx) {
    if (sortedIdx < 0 || sortedIdx >= _sortedFrameOriginalIndicesCache.length) {
      return null;
    }
    return _sortedFrameOriginalIndicesCache[sortedIdx];
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
    final max = (_sortedFramesCache.length - _visibleFrameCount).clamp(
      0.0,
      double.infinity,
    );
    _chartOffset = _chartOffset.clamp(0.0, max);
  }
}
