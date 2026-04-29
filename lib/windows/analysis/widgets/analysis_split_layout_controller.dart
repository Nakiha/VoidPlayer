import 'package:flutter/foundation.dart';

class AnalysisSplitLayoutController extends ChangeNotifier {
  double _topPanelFraction = 0.40;
  double _naluBrowserFraction = 0.42;

  double get topPanelFraction => _topPanelFraction;
  double get naluBrowserFraction => _naluBrowserFraction;

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
}
