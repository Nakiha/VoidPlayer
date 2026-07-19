import 'package:flutter/material.dart';

import '../analysis/ui/workspace/analysis_workspace_page.dart';
import '../l10n/app_localizations.dart';
import '../widgets/loop_range_bar.dart';
import '../widgets/resizable_divider.dart';
import '../widgets/timeline_area.dart';
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
const double _workspaceResizeHandleHeight = 9.0;
const double _workspaceHeaderHeight = 40.0;

class MainWindowDeck extends StatefulWidget {
  final MainWindowViewModel model;
  final MainWindowViewHandles handles;
  final MainWindowViewActions actions;

  const MainWindowDeck({
    super.key,
    required this.model,
    required this.handles,
    required this.actions,
  });

  @override
  State<MainWindowDeck> createState() => _MainWindowDeckState();
}

class _MainWindowDeckState extends State<MainWindowDeck> {
  bool _analysisActivated = false;

  @override
  void initState() {
    super.initState();
    _analysisActivated = widget.model.deck.tab == MainWindowDeckTab.analysis;
  }

  @override
  void didUpdateWidget(covariant MainWindowDeck oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.model.deck.tab == MainWindowDeckTab.analysis) {
      _analysisActivated = true;
    }
  }

  @override
  Widget build(BuildContext context) {
    final deck = widget.model.deck;
    final timelineVisible = deck.tab == MainWindowDeckTab.timeline;
    final compactTimelineHeight =
        _timelineChromeHeight +
        widget.model.media.tracks.length
                .clamp(0, _maxVisibleCompactTimelineTracks)
                .toDouble() *
            timelineTrackRowHeight;
    final maxHeight = (MediaQuery.sizeOf(context).height - 260)
        .clamp(kMinDeckHeight, kMaxDeckHeight)
        .toDouble();
    final workspaceHeight = deck.height
        .clamp(kMinDeckHeight, maxHeight)
        .toDouble();
    return SizedBox(
      height: timelineVisible ? compactTimelineHeight : workspaceHeight,
      child: Stack(
        fit: StackFit.expand,
        children: [
          Offstage(
            offstage: !timelineVisible,
            child: _TimelineDeckContent(
              model: widget.model,
              handles: widget.handles,
              actions: widget.actions,
            ),
          ),
          Offstage(
            offstage: timelineVisible,
            child: Column(
              children: [
                SizedBox(
                  key: mainWindowDeckResizeHandleKey,
                  height: _workspaceResizeHandleHeight,
                  child: ResizableHorizontalDivider(
                    value: workspaceHeight,
                    minValue: kMinDeckHeight,
                    maxValue: maxHeight,
                    deltaScale: -1,
                    onValueChanged: widget.actions.deck.onHeightChanged,
                  ),
                ),
                SizedBox(
                  height: _workspaceHeaderHeight - _workspaceResizeHandleHeight,
                  child: _AnalysisWorkspaceHeader(
                    selectedTab: deck.tab,
                    onTabChanged: (tab) {
                      if (tab != deck.tab) {
                        widget.actions.deck.onTabChanged(tab);
                      }
                    },
                    onClose: () => widget.actions.deck.onTabChanged(
                      MainWindowDeckTab.timeline,
                    ),
                  ),
                ),
                Expanded(
                  child: IndexedStack(
                    index: deck.tab == MainWindowDeckTab.quality ? 1 : 0,
                    children: [
                      if (_analysisActivated)
                        AnalysisWorkspacePage(
                          entries: widget.model.deck.analysisEntries,
                          testHosts: widget.model.deck.analysisTestHosts,
                          onSelectionChanged:
                              widget.actions.deck.onAnalysisSelectionChanged,
                          currentPlaybackByFileId:
                              widget.model.deck.analysisPlaybackByFileId,
                          onFrameSeekRequested:
                              widget.actions.deck.onAnalysisFrameSeekRequested,
                        )
                      else
                        const SizedBox.shrink(),
                      MainWindowQualityDeck(
                        model: widget.model,
                        actions: widget.actions,
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _AnalysisWorkspaceHeader extends StatelessWidget {
  final MainWindowDeckTab selectedTab;
  final ValueChanged<MainWindowDeckTab> onTabChanged;
  final VoidCallback onClose;

  const _AnalysisWorkspaceHeader({
    required this.selectedTab,
    required this.onTabChanged,
    required this.onClose,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colors = theme.colorScheme;
    return DecoratedBox(
      decoration: BoxDecoration(
        color: colors.surfaceContainerLowest,
        border: Border(bottom: BorderSide(color: theme.dividerColor)),
      ),
      child: Row(
        children: [
          const SizedBox(width: 8),
          for (final tab in const [
            MainWindowDeckTab.analysis,
            MainWindowDeckTab.quality,
          ])
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 2, vertical: 3),
              child: Semantics(
                selected: tab == selectedTab,
                button: true,
                child: TextButton(
                  key: ValueKey('main-window-deck-tab-${tab.name}'),
                  onPressed: () => onTabChanged(tab),
                  style: TextButton.styleFrom(
                    foregroundColor: tab == selectedTab
                        ? colors.onPrimaryContainer
                        : colors.onSurfaceVariant,
                    backgroundColor: tab == selectedTab
                        ? colors.primaryContainer
                        : Colors.transparent,
                    minimumSize: const Size(64, 30),
                    padding: const EdgeInsets.symmetric(horizontal: 12),
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    visualDensity: VisualDensity.compact,
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(6),
                    ),
                  ),
                  child: Text(
                    _labelFor(context, tab),
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                  ),
                ),
              ),
            ),
          const Spacer(),
          IconButton(
            key: mainWindowDeckCollapseButtonKey,
            onPressed: onClose,
            tooltip: MaterialLocalizations.of(context).closeButtonTooltip,
            icon: const Icon(Icons.close),
          ),
        ],
      ),
    );
  }

  String _labelFor(BuildContext context, MainWindowDeckTab tab) {
    final l = AppLocalizations.of(context)!;
    return switch (tab) {
      MainWindowDeckTab.analysis => l.deckAnalysisTab,
      MainWindowDeckTab.quality => l.deckQualityTab,
      MainWindowDeckTab.timeline => l.deckTimelineTab,
    };
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
