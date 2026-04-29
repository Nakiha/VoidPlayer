import 'package:flutter/material.dart';

import '../../../l10n/app_localizations.dart';
import '../charts/analysis_charts.dart';
import '../widgets/analysis_split_layout_controller.dart';
import '../widgets/analysis_controls.dart';
import '../widgets/analysis_nalu.dart';
import '../widgets/analysis_style.dart';
import 'analysis_page_state.dart';

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
            currentIdx: model.currentSortedFrameIdx,
            selectedFrameIdx: model.selectedSortedFrameIdx,
            pocToIndices: model.sortedPocToIndices,
            onFrameSelected: actions.onChartFrameSelected,
            viewStart: model.chartOffset,
            viewEnd: model.chartOffset + model.visibleFrameCount,
            ptsOrder: model.ptsOrder,
            onZoom: actions.onChartZoom,
            onPan: actions.onChartPan,
            l: l,
          )
        : AnalysisFrameTrendView(
            frames: model.sortedFrames,
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
            l: l,
          );
    final bottomPanel = LayoutBuilder(
      builder: (context, constraints) {
        final totalW = constraints.maxWidth;
        final maxBrowserW = (totalW - 120).clamp(120.0, double.infinity);
        final layoutController = splitLayoutController;
        final requestedBrowserW = layoutController != null
            ? totalW * layoutController.naluBrowserFraction
            : model.naluBrowserWidth;
        final browserW = requestedBrowserW.clamp(120.0, maxBrowserW);
        return Stack(
          children: [
            Row(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                SizedBox(
                  width: browserW,
                  child: AnalysisNaluBrowserView(
                    nalus: model.nalus,
                    codec: model.codec,
                    selectedIdx: model.selectedNaluIdx,
                    onSelected: actions.onNaluSelected,
                    filter: model.naluFilter,
                    onFilterChanged: actions.onNaluFilterChanged,
                  ),
                ),
                Container(width: 1, color: theme.colorScheme.outlineVariant),
                Expanded(
                  child: AnalysisNaluDetailView(
                    nalu:
                        model.selectedNaluIdx != null &&
                            model.selectedNaluIdx! < model.nalus.length
                        ? model.nalus[model.selectedNaluIdx!]
                        : null,
                    frameIdx: model.selectedFrameIdx,
                    frames: model.frames,
                    codec: model.codec,
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
                    actions.onNaluBrowserWidthChanged(clamped);
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
          Container(
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
          Expanded(
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
                final layoutController = splitLayoutController;
                final topPanelFraction =
                    layoutController?.topPanelFraction ??
                    model.topPanelFraction;
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
                            actions.onTopPanelFractionChanged(nextFraction);
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
