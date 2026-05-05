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
    final media = model.media;
    final playback = model.playback;
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        MainWindowMediaHeader(model: model, actions: actions),
        MainWindowControlsBar(model: model, actions: actions),
        LoopRangeBar(
          key: playback.loopRangeBarKey,
          timelineStartWidth: playback.timelineStartWidth,
          enabled: playback.loopRangeEnabled,
          startUs: playback.loopStartUs,
          endUs: playback.loopEndUs,
          durationUs: playback.durationUs,
          onEnabledChanged: actions.onLoopRangeEnabledChanged,
          onRangeChanged: actions.onLoopRangeChanged,
          onRangeChangeEnd: actions.onLoopRangeChangeEnd,
        ),
        ValueListenableBuilder<TimelineHoverState>(
          valueListenable: playback.timelineHoverListenable,
          builder: (context, hover, _) => TimelineArea(
            entries: media.tracks,
            currentPtsUs: playback.currentPtsUs,
            onRemoveTrack: actions.onRemoveTrack,
            onReorder: actions.onReorder,
            onOffsetChanged: actions.onOffsetChanged,
            onToggleTrackAudio: actions.onToggleTrackAudio,
            audibleTrackFileId: media.audibleTrackFileId,
            syncOffsets: media.syncOffsets,
            maxEffectiveDurationUs: playback.durationUs,
            hoverPtsUs: hover.hoverPtsUs,
            sliderHovering: hover.sliderHovering,
            controlsWidth: playback.controlsWidth,
            onControlsWidthChanged: actions.onControlsWidthChanged,
            markerPtsUs: playback.markerUs,
            loopRangeEnabled: playback.loopRangeEnabled,
            loopStartUs: playback.loopStartUs,
            loopEndUs: playback.loopEndUs,
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
    final tracks = model.media.tracks;
    return MediaHeaderBar(
      entries: tracks,
      onMediaSwapped: actions.onMediaSwapped,
      onRemoveClicked: (slotIndex) {
        if (slotIndex < tracks.length) {
          actions.onRemoveTrack(tracks[slotIndex].fileId);
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
    final playback = model.playback;
    return ControlsBar(
      timelineKey: playback.timelineSliderKey,
      timelineStartWidth: playback.timelineStartWidth,
      zoomRatio: model.viewport.layout.zoomRatio,
      onZoomChanged: actions.onZoomChanged,
      isPlaying: playback.isPlaying,
      isFullScreen: model.overlays.fullScreen,
      onToggleFullScreen: actions.onToggleFullScreen,
      onTogglePlay: actions.onTogglePlay,
      onStepForward: actions.onStepForward,
      onStepBackward: actions.onStepBackward,
      currentPtsUs: playback.currentPtsUs,
      durationUs: playback.durationUs,
      onSeek: actions.onSeek,
      onHoverChanged: actions.onSliderHover,
      markerUs: playback.markerUs,
      seekMinUs: playback.seekMinUs,
      seekMaxUs: playback.seekMaxUs,
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
