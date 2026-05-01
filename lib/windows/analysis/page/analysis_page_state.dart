import 'package:flutter/foundation.dart';

import '../../../analysis/analysis_ffi.dart';
import '../../../analysis/nalu_types.dart';

enum AnalysisFrameTrendAxis { frameSize, qp }

class AnalysisPageViewModel {
  final int selectedTab;
  final bool ptsOrder;
  final int? selectedNaluIdx;
  final String naluFilter;
  final double naluBrowserWidth;
  final int? selectedFrameIdx;
  final double visibleFrameCount;
  final double chartOffset;
  final double frameSizeAxisZoom;
  final double qpAxisZoom;
  final double topPanelFraction;
  final List<FrameInfo> frames;
  final List<NaluInfo> nalus;
  final List<FrameInfo> sortedFrames;
  final Map<int, List<int>> sortedPocToIndices;
  final AnalysisSummary? summary;
  final AnalysisCodec codec;
  final int? selectedSortedFrameIdx;
  final int currentSortedFrameIdx;

  const AnalysisPageViewModel({
    required this.selectedTab,
    required this.ptsOrder,
    required this.selectedNaluIdx,
    required this.naluFilter,
    required this.naluBrowserWidth,
    required this.selectedFrameIdx,
    required this.visibleFrameCount,
    required this.chartOffset,
    required this.frameSizeAxisZoom,
    required this.qpAxisZoom,
    required this.topPanelFraction,
    required this.frames,
    required this.nalus,
    required this.sortedFrames,
    required this.sortedPocToIndices,
    required this.summary,
    required this.codec,
    required this.selectedSortedFrameIdx,
    required this.currentSortedFrameIdx,
  });
}

class AnalysisPageActions {
  final ValueChanged<bool> onOrderChanged;
  final ValueChanged<int> onTabChanged;
  final ValueChanged<double> onChartZoom;
  final ValueChanged<double> onChartPan;
  final void Function(AnalysisFrameTrendAxis axis, double scrollDelta)
  onAxisZoom;
  final ValueChanged<int?> onChartFrameSelected;
  final ValueChanged<int?> onNaluSelected;
  final ValueChanged<String> onNaluFilterChanged;
  final ValueChanged<double> onNaluBrowserWidthChanged;
  final ValueChanged<double> onTopPanelFractionChanged;

  const AnalysisPageActions({
    required this.onOrderChanged,
    required this.onTabChanged,
    required this.onChartZoom,
    required this.onChartPan,
    required this.onAxisZoom,
    required this.onChartFrameSelected,
    required this.onNaluSelected,
    required this.onNaluFilterChanged,
    required this.onNaluBrowserWidthChanged,
    required this.onTopPanelFractionChanged,
  });
}
