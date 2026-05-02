import 'package:flutter/widgets.dart';

import '../../../analysis/analysis_ffi.dart';
import '../../../analysis/nalu_types.dart';

abstract class AnalysisTestHost {
  bool get mounted;
  List<FrameInfo> get analysisFrames;
  List<NaluInfo> get analysisNalus;
  int get analysisFrameIndexBase;
  int get analysisNaluIndexBase;
  AnalysisSummary? get analysisSummary;
  AnalysisCodec get analysisCodec;
  int? get selectedAnalysisFrameIdx;
  int? get selectedAnalysisNaluIdx;
  double get analysisChartOffset;
  double get analysisVisibleFrameCount;
  int get analysisSelectedTab;
  bool get analysisPtsOrder;
  bool get isAnalysisLoaded;

  void readAnalysisDataForTest();
  int? sortedPositionForFrameIdx(int frameIdx);
  void updateAnalysisTestState(VoidCallback update);
  void setAnalysisTabForTest(int tab);
  void setAnalysisOrderForTest(bool ptsOrder);
  void setAnalysisChartWindowForTest(double offset, double visibleFrameCount);
  void selectAnalysisNaluForTest(int naluIdx);
}
