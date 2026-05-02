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
  final int frameIndexBase;
  final int totalFrameCount;
  final List<FrameBucket> frameBuckets;
  final int frameBucketSize;
  final List<NaluInfo> nalus;
  final int naluIndexBase;
  final int totalNaluCount;
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
    required this.frameIndexBase,
    required this.totalFrameCount,
    required this.frameBuckets,
    required this.frameBucketSize,
    required this.nalus,
    required this.naluIndexBase,
    required this.totalNaluCount,
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
  final void Function(int start, int count) onNaluWindowRequested;
  final void Function(double offset, double visibleFrameCount)
  onChartWindowSetForTest;
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
    required this.onNaluWindowRequested,
    required this.onChartWindowSetForTest,
    required this.onNaluFilterChanged,
    required this.onNaluBrowserWidthChanged,
    required this.onTopPanelFractionChanged,
  });
}
