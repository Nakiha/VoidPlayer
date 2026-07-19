import 'package:flutter/material.dart';

import '../widgets/loop_range_bar.dart';
import '../widgets/timeline_area.dart';
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

class MainWindowDeck extends StatelessWidget {
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
  Widget build(BuildContext context) {
    final compactTimelineHeight =
        _timelineChromeHeight +
        model.media.tracks.length
                .clamp(0, _maxVisibleCompactTimelineTracks)
                .toDouble() *
            timelineTrackRowHeight;
    return SizedBox(
      height: compactTimelineHeight,
      child: _TimelineDeckContent(
        model: model,
        handles: handles,
        actions: actions,
      ),
    );
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
