import 'package:flutter/material.dart';

import '../../widgets/controls_bar.dart';
import '../../widgets/loop_range_bar.dart';
import '../../widgets/media_header.dart';
import '../../widgets/timeline_area.dart';
import 'main_window_state.dart';
import 'main_window_view_model.dart';

class MediaTimelineSection extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const MediaTimelineSection({
    super.key,
    required this.model,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        MainWindowMediaHeader(model: model, actions: actions),
        MainWindowControlsBar(model: model, actions: actions),
        LoopRangeBar(
          key: model.loopRangeBarKey,
          timelineStartWidth: model.timelineStartWidth,
          enabled: model.loopRangeEnabled,
          startUs: model.loopStartUs,
          endUs: model.loopEndUs,
          durationUs: model.durationUs,
          onEnabledChanged: actions.onLoopRangeEnabledChanged,
          onRangeChanged: actions.onLoopRangeChanged,
          onRangeChangeEnd: actions.onLoopRangeChangeEnd,
        ),
        ValueListenableBuilder<TimelineHoverState>(
          valueListenable: model.timelineHoverListenable,
          builder: (context, hover, _) => TimelineArea(
            entries: model.tracks,
            currentPtsUs: model.currentPtsUs,
            onRemoveTrack: actions.onRemoveTrack,
            onReorder: actions.onReorder,
            onOffsetChanged: actions.onOffsetChanged,
            onToggleTrackAudio: actions.onToggleTrackAudio,
            audibleTrackFileId: model.audibleTrackFileId,
            syncOffsets: model.syncOffsets,
            maxEffectiveDurationUs: model.durationUs,
            hoverPtsUs: hover.hoverPtsUs,
            sliderHovering: hover.sliderHovering,
            controlsWidth: model.controlsWidth,
            onControlsWidthChanged: actions.onControlsWidthChanged,
            markerPtsUs: model.markerUs,
            loopRangeEnabled: model.loopRangeEnabled,
            loopStartUs: model.loopStartUs,
            loopEndUs: model.loopEndUs,
          ),
        ),
      ],
    );
  }
}

class MainWindowMediaHeader extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const MainWindowMediaHeader({
    super.key,
    required this.model,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    return MediaHeaderBar(
      entries: model.tracks,
      onMediaSwapped: actions.onMediaSwapped,
      onRemoveClicked: (slotIndex) {
        if (slotIndex < model.tracks.length) {
          actions.onRemoveTrack(model.tracks[slotIndex].fileId);
        }
      },
    );
  }
}

class MainWindowControlsBar extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const MainWindowControlsBar({
    super.key,
    required this.model,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    return ControlsBar(
      timelineKey: model.timelineSliderKey,
      timelineStartWidth: model.timelineStartWidth,
      zoomRatio: model.layout.zoomRatio,
      onZoomChanged: actions.onZoomChanged,
      isPlaying: model.isPlaying,
      isFullScreen: model.fullScreen,
      onToggleFullScreen: actions.onToggleFullScreen,
      onTogglePlay: actions.onTogglePlay,
      onStepForward: actions.onStepForward,
      onStepBackward: actions.onStepBackward,
      currentPtsUs: model.currentPtsUs,
      durationUs: model.durationUs,
      onSeek: actions.onSeek,
      onHoverChanged: actions.onSliderHover,
      markerUs: model.markerUs,
      seekMinUs: model.seekMinUs,
      seekMaxUs: model.seekMaxUs,
    );
  }
}

class FullScreenControlsPanel extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const FullScreenControlsPanel({
    super.key,
    required this.model,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return MouseRegion(
      onEnter: (_) => actions.onFullScreenControlsHoverChanged(true),
      onExit: (_) => actions.onFullScreenControlsHoverChanged(false),
      child: Material(
        color: Colors.transparent,
        child: DecoratedBox(
          decoration: BoxDecoration(
            color: colorScheme.surface.withValues(alpha: 0.86),
            borderRadius: BorderRadius.circular(8),
            border: Border.all(
              color: colorScheme.outlineVariant.withValues(alpha: 0.72),
            ),
          ),
          child: Padding(
            padding: const EdgeInsets.all(4),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                MainWindowMediaHeader(model: model, actions: actions),
                MainWindowControlsBar(model: model, actions: actions),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
