import 'package:flutter/material.dart';

import '../analysis/ui/page/analysis_page_state.dart';
import '../analysis/ui/workspace/analysis_workspace_models.dart';
import '../analysis/ui/workspace/analysis_workspace_page.dart';
import '../native_player/native_player_protocol.dart';
import '../widgets/loop_range_bar.dart';
import '../widgets/timeline_area.dart';
import 'main_window_analysis_dock.dart';
import 'main_window_list_sidebar.dart';
import 'main_window_quality.dart';
import 'main_window_state.dart';
import 'main_window_view_handles.dart';
import 'main_window_view_model.dart';

const Key mainWindowDeckResizeHandleKey = ValueKey(
  'main-window-deck-resize-handle',
);
const Key mainWindowDeckCollapseButtonKey = ValueKey(
  'main-window-deck-collapse-button',
);
const double _timelineChromeHeight = 32.0;
const int _maxVisibleCompactTimelineTracks = 4;
const double _defaultAnalysisDeckFraction = 0.42;
const double _minAnalysisDeckFraction = 0.25;
const double _maxAnalysisDeckFraction = 0.70;
const double _analysisDeckResizeHandleHeight = 8.0;

class MainWindowDeck extends StatefulWidget {
  final MainWindowViewModel model;
  final MainWindowViewHandles handles;
  final MainWindowViewActions actions;
  final double availableHeight;
  final ValueNotifier<MainWindowAnalysisFocus?> analysisFocus;

  const MainWindowDeck({
    super.key,
    required this.model,
    required this.handles,
    required this.actions,
    required this.availableHeight,
    required this.analysisFocus,
  });

  @override
  State<MainWindowDeck> createState() => _MainWindowDeckState();
}

class _MainWindowDeckState extends State<MainWindowDeck> {
  double _analysisDeckFraction = _defaultAnalysisDeckFraction;
  final MainWindowQualitySession _qualitySession = MainWindowQualitySession();
  int _focusPublication = 0;

  @override
  Widget build(BuildContext context) {
    final model = widget.model;
    final compactTimelineHeight =
        _timelineChromeHeight +
        model.media.tracks.length
                .clamp(0, _maxVisibleCompactTimelineTracks)
                .toDouble() *
            timelineTrackRowHeight;
    if (model.deck.tab == MainWindowDeckTab.timeline) {
      _publishAnalysisFocus(null);
      return SizedBox(
        height: compactTimelineHeight,
        child: _TimelineDeckContent(
          model: model,
          handles: widget.handles,
          actions: widget.actions,
        ),
      );
    }

    final deckHeight = widget.availableHeight * _analysisDeckFraction;
    return SizedBox(
      height: deckHeight,
      child: Column(
        children: [
          MouseRegion(
            cursor: SystemMouseCursors.resizeUpDown,
            child: GestureDetector(
              key: mainWindowDeckResizeHandleKey,
              behavior: HitTestBehavior.opaque,
              onVerticalDragUpdate: _resizeAnalysisDeck,
              child: SizedBox(
                height: _analysisDeckResizeHandleHeight,
                child: Center(
                  child: Divider(
                    height: 1,
                    color: Theme.of(context).colorScheme.outlineVariant,
                  ),
                ),
              ),
            ),
          ),
          _buildAnalysisHeader(),
          Expanded(
            key: mainWindowAnalysisChartShelfKey,
            child: AnalysisWorkspacePage(
              entries: model.deck.analysisEntries,
              testHosts: model.deck.analysisTestHosts,
              onSelectionChanged:
                  widget.actions.deck.onAnalysisSelectionChanged,
              currentPlaybackByFileId: model.deck.analysisPlaybackByFileId,
              onFrameSeekRequested:
                  widget.actions.deck.onAnalysisFrameSeekRequested,
              splitView: model.viewport.viewMode == LayoutMode.splitScreen,
              contentBuilder:
                  (
                    context,
                    entry,
                    entries,
                    selectedIndex,
                    onSelected,
                    analysisModel,
                    analysisActions,
                  ) => _buildAnalysisDock(
                    entry: entry,
                    entries: entries,
                    selectedIndex: selectedIndex,
                    onSelected: onSelected,
                    analysisModel: analysisModel,
                    analysisActions: analysisActions,
                  ),
              fallbackBuilder:
                  (context, entries, selectedIndex, onSelected, entry) =>
                      _buildPendingDock(
                        entry: entry,
                        entries: entries,
                        selectedIndex: selectedIndex,
                        onSelected: onSelected,
                      ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildAnalysisDock({
    required AnalysisWorkspaceEntry entry,
    required List<AnalysisWorkspaceEntry> entries,
    required int selectedIndex,
    required ValueChanged<int> onSelected,
    required AnalysisPageViewModel analysisModel,
    required AnalysisPageActions analysisActions,
  }) {
    if (entries[selectedIndex].fileId == entry.fileId) {
      _publishAnalysisFocus(
        MainWindowAnalysisFocus(
          entry: entry,
          entries: entries,
          selectedIndex: selectedIndex,
          onSelected: onSelected,
          pageModel: analysisModel,
          pageActions: analysisActions,
        ),
      );
    }
    return MainWindowAnalysisDock(
      entry: entry,
      analysisModel: analysisModel,
      analysisActions: analysisActions,
      model: widget.model,
      actions: widget.actions,
      qualitySession: _qualitySession,
    );
  }

  Widget _buildPendingDock({
    required AnalysisWorkspaceEntry? entry,
    required List<AnalysisWorkspaceEntry> entries,
    required int selectedIndex,
    required ValueChanged<int> onSelected,
  }) {
    if (entry == null || entries[selectedIndex].fileId == entry.fileId) {
      _publishAnalysisFocus(
        MainWindowAnalysisFocus(
          entry: entry,
          entries: entries,
          selectedIndex: selectedIndex,
          onSelected: onSelected,
        ),
      );
    }
    return MainWindowAnalysisPendingDock(
      entry: entry,
      model: widget.model,
      actions: widget.actions,
      qualitySession: _qualitySession,
    );
  }

  Widget _buildAnalysisHeader() {
    return ValueListenableBuilder<MainWindowAnalysisFocus?>(
      valueListenable: widget.analysisFocus,
      builder: (context, focus, _) {
        final entries =
            focus?.entries ?? widget.model.deck.analysisEntries.value;
        final qualitySelected =
            widget.model.deck.tab == MainWindowDeckTab.quality;
        final selectedIndex = entries.isEmpty
            ? 0
            : (focus?.selectedIndex ?? 0).clamp(0, entries.length - 1).toInt();

        void selectAnalysisTab(int tab) {
          focus?.pageActions?.onTabChanged(tab);
          if (qualitySelected) {
            widget.actions.deck.onTabChanged(MainWindowDeckTab.analysis);
          }
        }

        return MainWindowAnalysisDeckHeader(
          entries: entries,
          selectedIndex: selectedIndex,
          showTrackSelector:
              widget.model.viewport.viewMode != LayoutMode.splitScreen,
          onTrackSelected: focus?.onSelected,
          selectedAnalysisTab: focus?.pageModel?.selectedTab ?? 0,
          qualitySelected: qualitySelected,
          onReferencePressed: () => selectAnalysisTab(0),
          onTrendPressed: () => selectAnalysisTab(1),
          onQualityPressed: () =>
              widget.actions.deck.onTabChanged(MainWindowDeckTab.quality),
          onClose: () =>
              widget.actions.deck.onTabChanged(MainWindowDeckTab.timeline),
          closeButtonKey: mainWindowDeckCollapseButtonKey,
        );
      },
    );
  }

  void _publishAnalysisFocus(MainWindowAnalysisFocus? focus) {
    final publication = ++_focusPublication;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || publication != _focusPublication) return;
      widget.analysisFocus.value = focus;
    });
  }

  void _resizeAnalysisDeck(DragUpdateDetails details) {
    final availableHeight = widget.availableHeight;
    if (availableHeight <= 0) return;
    final next = (_analysisDeckFraction - details.delta.dy / availableHeight)
        .clamp(_minAnalysisDeckFraction, _maxAnalysisDeckFraction)
        .toDouble();
    if (next == _analysisDeckFraction) return;
    setState(() => _analysisDeckFraction = next);
    widget.actions.deck.onHeightChanged(availableHeight * next);
  }
}

class _TimelineDeckContent extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewHandles handles;
  final MainWindowViewActions actions;

  const _TimelineDeckContent({
    required this.model,
    required this.handles,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    final media = model.media;
    final playback = model.playback;
    final mediaActions = actions.mediaTimeline;
    return Column(
      children: [
        LoopRangeBar(
          key: handles.loopRangeBarKey,
          timelineStartWidth: playback.timelineStartWidth,
          enabled: playback.loopRangeEnabled,
          startUs: playback.loopStartUs,
          endUs: playback.loopEndUs,
          durationUs: playback.durationUs,
          onEnabledChanged: mediaActions.onLoopRangeEnabledChanged,
          onRangeChanged: mediaActions.onLoopRangeChanged,
          onRangeChangeEnd: mediaActions.onLoopRangeChangeEnd,
        ),
        Expanded(
          child: ValueListenableBuilder<TimelineHoverState>(
            valueListenable: handles.timelineHoverListenable,
            builder: (context, hover, _) => TimelineArea(
              entries: media.tracks,
              currentPtsUs: playback.currentPtsUs,
              onRemoveTrack: mediaActions.onRemoveTrack,
              onReorder: mediaActions.onReorder,
              onOffsetChanged: mediaActions.onOffsetChanged,
              onToggleTrackAudio: mediaActions.onToggleTrackAudio,
              canRemoveTrack: model.session.capabilities.canRemoveTrack,
              canReorderTrack: model.session.capabilities.canReorderTrack,
              canAdjustTrackOffset:
                  model.session.capabilities.canAdjustTrackOffset,
              canToggleTrackAudio:
                  model.session.capabilities.canToggleTrackAudio,
              audibleTrackFileId: media.audibleTrackFileId,
              syncOffsets: media.syncOffsets,
              maxEffectiveDurationUs: playback.durationUs,
              hoverPtsUs: hover.hoverPtsUs,
              sliderHovering: hover.sliderHovering,
              controlsWidth: playback.controlsWidth,
              onControlsWidthChanged: mediaActions.onControlsWidthChanged,
              markerPtsUs: playback.markerUs,
              loopRangeEnabled: playback.loopRangeEnabled,
              loopStartUs: playback.loopStartUs,
              loopEndUs: playback.loopEndUs,
            ),
          ),
        ),
      ],
    );
  }
}
