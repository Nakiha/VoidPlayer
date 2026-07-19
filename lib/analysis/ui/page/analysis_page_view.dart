import 'package:flutter/material.dart';

import '../../../l10n/app_localizations.dart';
import '../../../widgets/axtree_region.dart';
import '../charts/analysis_charts.dart';
import '../widgets/analysis_controls.dart';
import '../widgets/analysis_nalu.dart';
import '../widgets/analysis_split_layout_controller.dart';
import '../widgets/analysis_style.dart';
import 'analysis_page_state.dart';

const Key analysisNaluBrowserPanelKey = ValueKey('analysis-nalu-browser-panel');
const Key analysisNaluDetailPanelKey = ValueKey('analysis-nalu-detail-panel');

class AnalysisPageView extends StatelessWidget {
  final AnalysisPageViewModel model;
  final AnalysisPageActions actions;
  final AnalysisSplitLayoutController? splitLayoutController;

  const AnalysisPageView({
    super.key,
    required this.model,
    required this.actions,
    this.splitLayoutController,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    final topChart = model.selectedTab == 0
        ? AnalysisReferencePyramidView(
            frames: model.sortedFrames,
            frameIndexBase: model.frameIndexBase,
            totalFrames: model.totalFrameCount,
            currentIdx: model.currentSortedFrameIdx,
            selectedFrameIdx: model.selectedSortedFrameIdx,
            pocToIndices: model.sortedPocToIndices,
            useActualTemporalLayers: model.referencePyramidActualTemporalLayers,
            onLayerModeChanged: actions.onReferencePyramidLayerModeChanged,
            onFrameSelected: actions.onChartFrameSelected,
            onFrameActivated: actions.onChartFrameActivated,
            viewStart: model.chartOffset,
            viewEnd: model.chartOffset + model.visibleFrameCount,
            ptsOrder: model.ptsOrder,
            onZoom: actions.onChartZoom,
            onPan: actions.onChartPan,
            l: l,
          )
        : AnalysisFrameTrendView(
            frames: model.sortedFrames,
            frameIndexBase: model.frameIndexBase,
            totalFrames: model.totalFrameCount,
            frameBuckets: model.frameBuckets,
            frameBucketSize: model.frameBucketSize,
            currentIdx: model.currentSortedFrameIdx,
            selectedFrameIdx: model.selectedSortedFrameIdx,
            viewStart: model.chartOffset,
            viewEnd: model.chartOffset + model.visibleFrameCount,
            frameSizeAxisZoom: model.frameSizeAxisZoom,
            qpAxisZoom: model.qpAxisZoom,
            ptsOrder: model.ptsOrder,
            onZoom: actions.onChartZoom,
            onAxisZoom: actions.onAxisZoom,
            onPan: actions.onChartPan,
            onFrameSelected: actions.onChartFrameSelected,
            onFrameActivated: actions.onChartFrameActivated,
            l: l,
          );
    final bottomPanel = AnalysisNaluBrowserView(
      key: analysisNaluBrowserPanelKey,
      nalus: model.nalus,
      naluIndexBase: model.naluIndexBase,
      totalNalus: model.totalNaluCount,
      codec: model.codec,
      selectedIdx: model.selectedNaluIdx,
      onSelected: actions.onNaluSelected,
      onWindowRequested: actions.onNaluWindowRequested,
      filter: model.naluFilter,
      onFilterChanged: actions.onNaluFilterChanged,
    );
    return Scaffold(
      body: AxTreeRegion(
        label: 'Analysis window',
        child: Column(
          children: [
            AxTreeRegion(
              label: 'Analysis toolbar',
              child: Container(
                height: analysisHeaderHeight,
                padding: analysisHeaderPadding,
                decoration: BoxDecoration(
                  border: Border(bottom: BorderSide(color: theme.dividerColor)),
                ),
                child: Row(
                  children: [
                    SizedBox(
                      height: analysisHeaderControlHeight,
                      child: AnalysisOrderToggle(
                        ptsOrder: model.ptsOrder,
                        onChanged: actions.onOrderChanged,
                        l: l,
                      ),
                    ),
                    const Spacer(),
                    SizedBox(
                      height: analysisHeaderControlHeight,
                      child: AnalysisViewTabBar(
                        selectedTab: model.selectedTab,
                        onTabChanged: actions.onTabChanged,
                        l: l,
                      ),
                    ),
                  ],
                ),
              ),
            ),
            Expanded(
              child: LayoutBuilder(
                builder: (context, constraints) {
                  const dividerHitH = 10.0;
                  final available = constraints.maxHeight.clamp(
                    0.0,
                    double.infinity,
                  );
                  const dividerLineH = 1.0;
                  final contentAvailable = (available - dividerLineH).clamp(
                    0.0,
                    double.infinity,
                  );
                  final compact = contentAvailable < 280;
                  final minTop = compact ? contentAvailable * 0.28 : 120.0;
                  final minBottom = compact ? contentAvailable * 0.28 : 170.0;
                  final maxTop = (contentAvailable - minBottom).clamp(
                    minTop,
                    contentAvailable,
                  );
                  final layoutController = splitLayoutController;
                  final topPanelFraction =
                      layoutController?.topPanelFraction ??
                      model.topPanelFraction;
                  final topH = (contentAvailable * topPanelFraction).clamp(
                    minTop,
                    maxTop,
                  );
                  final bottomH = contentAvailable - topH;
                  final dividerTop = topH.clamp(
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
                        top: topH + dividerLineH,
                        height: bottomH,
                        child: AxTreeRegion(
                          label: 'NAL unit browser',
                          child: bottomPanel,
                        ),
                      ),
                      Positioned(
                        left: 0,
                        right: 0,
                        top: dividerTop,
                        height: dividerHitH,
                        child: ExcludeSemantics(
                          child: AnalysisResizableHDivider(
                            position: topH,
                            minPosition: minTop,
                            maxPosition: maxTop,
                            onPositionChanged: (nextTop) {
                              if (contentAvailable <= 0) return;
                              final nextFraction = nextTop / contentAvailable;
                              if (layoutController != null) {
                                layoutController.setTopPanelFraction(
                                  nextFraction,
                                );
                              } else {
                                actions.onTopPanelFractionChanged(nextFraction);
                              }
                            },
                          ),
                        ),
                      ),
                    ],
                  );
                },
              ),
            ),
          ],
        ),
      ),
    );
  }
}
