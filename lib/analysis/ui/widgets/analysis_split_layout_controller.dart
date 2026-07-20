import 'package:flutter/foundation.dart';

class AnalysisSplitLayoutController extends ChangeNotifier {
  double _topPanelFraction = 0.40;
  double _naluBrowserFraction = 0.42;
  double? _chartOffset;
  double? _visibleFrameCount;
  int? _chartWindowSourceFileId;
  int _chartWindowRevision = 0;
  int? _selectedTab;
  bool? _ptsOrder;
  int? _viewStateSourceFileId;
  int _viewStateRevision = 0;

  double get topPanelFraction => _topPanelFraction;
  double get naluBrowserFraction => _naluBrowserFraction;
  double? get chartOffset => _chartOffset;
  double? get visibleFrameCount => _visibleFrameCount;
  int? get chartWindowSourceFileId => _chartWindowSourceFileId;
  int get chartWindowRevision => _chartWindowRevision;
  int? get selectedTab => _selectedTab;
  bool? get ptsOrder => _ptsOrder;
  int? get viewStateSourceFileId => _viewStateSourceFileId;
  int get viewStateRevision => _viewStateRevision;

  void setTopPanelFraction(double value) {
    final next = value.clamp(0.0, 1.0);
    if ((next - _topPanelFraction).abs() < 0.0001) return;
    _topPanelFraction = next;
    notifyListeners();
  }

  void setNaluBrowserFraction(double value) {
    final next = value.clamp(0.0, 1.0);
    if ((next - _naluBrowserFraction).abs() < 0.0001) return;
    _naluBrowserFraction = next;
    notifyListeners();
  }

  void setChartWindow({
    required int sourceFileId,
    required double offset,
    required double visibleFrameCount,
  }) {
    if (_chartWindowSourceFileId == sourceFileId &&
        _chartOffset == offset &&
        _visibleFrameCount == visibleFrameCount) {
      return;
    }
    _chartWindowSourceFileId = sourceFileId;
    _chartOffset = offset;
    _visibleFrameCount = visibleFrameCount;
    _chartWindowRevision++;
    notifyListeners();
  }

  void setViewState({
    required int sourceFileId,
    required int selectedTab,
    required bool ptsOrder,
  }) {
    if (_viewStateSourceFileId == sourceFileId &&
        _selectedTab == selectedTab &&
        _ptsOrder == ptsOrder) {
      return;
    }
    _viewStateSourceFileId = sourceFileId;
    _selectedTab = selectedTab;
    _ptsOrder = ptsOrder;
    _viewStateRevision++;
    notifyListeners();
  }
}
