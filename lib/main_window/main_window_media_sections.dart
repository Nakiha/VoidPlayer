import 'package:flutter/material.dart';

import '../utils/async_guard.dart';
import '../widgets/analysis_overlay_controls.dart';
import '../widgets/controls_bar.dart';
import '../widgets/loop_range_bar.dart';
import '../widgets/media_header.dart';
import '../widgets/timeline_area.dart';
import 'main_window_state.dart';
import 'main_window_view_handles.dart';
import 'main_window_view_model.dart';

class MediaTimelineSection extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewHandles handles;
  final MainWindowViewActions actions;

  const MediaTimelineSection({
    super.key,
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
      mainAxisSize: MainAxisSize.min,
      children: [
        AnalysisOverlayStrip(
          entries: media.tracks,
          dataSource: media.analysisDataSource,
          visible:
              model.session.capabilities.canShowAnalysisOverlay &&
              media.analysisOverlayEnabled &&
              model.overlays.analysisOverlayControlsVisible,
          onTypeChanged: actions.analysisOverlay.onTypeChanged,
          onOpacityChanged: actions.analysisOverlay.onOpacityChanged,
          onActivateOverlay: actions.analysisOverlay.onActivate,
          onDeactivateOverlay: actions.analysisOverlay.onClose,
          onClose: () {
            actions.toolbar.onAnalysisOverlayPanelToggle();
          },
        ),
        MainWindowMediaHeader(model: model, handles: handles, actions: actions),
        MainWindowControlsBar(model: model, handles: handles, actions: actions),
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
        ValueListenableBuilder<TimelineHoverState>(
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
            canToggleTrackAudio: model.session.capabilities.canToggleTrackAudio,
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
      ],
    );
  }
}

class MainWindowMediaHeader extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewHandles handles;
  final MainWindowViewActions actions;

  const MainWindowMediaHeader({
    super.key,
    required this.model,
    required this.handles,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    final media = model.media;
    final tracks = media.tracks;
    final mediaActions = actions.mediaTimeline;
    final capabilities = model.session.capabilities;
    return MediaHeaderBar(
      entries: tracks,
      analysisOverlayEnabled:
          capabilities.canShowAnalysisOverlay && media.analysisOverlayEnabled,
      analysisOverlayControlsVisible:
          model.overlays.analysisOverlayControlsVisible,
      analysisDataSource: media.analysisDataSource,
      analysisOverlayButtonKey: handles.analysisOverlayButtonKey,
      canRemoveTrack: capabilities.canRemoveTrack,
      canReorderTrack: capabilities.canReorderTrack,
      onAnalysisOverlayControlsToggle: () {
        actions.toolbar.onAnalysisOverlayPanelToggle();
      },
      onMediaSwapped: mediaActions.onMediaSwapped,
      onRemoveClicked: (fileId) =>
          fireAndLog('remove track', mediaActions.onRemoveTrack(fileId)),
    );
  }
}

class MainWindowControlsBar extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewHandles handles;
  final MainWindowViewActions actions;

  const MainWindowControlsBar({
    super.key,
    required this.model,
    required this.handles,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    final playback = model.playback;
    final mediaActions = actions.mediaTimeline;
    return ControlsBar(
      key: handles.controlsBarKey,
      timelineKey: handles.timelineSliderKey,
      timelineStartWidth: playback.timelineStartWidth,
      zoomRatio: model.viewport.layout.zoomRatio,
      onZoomChanged: mediaActions.onZoomChanged,
      isPlaying: playback.isPlaying,
      isFullScreen: model.overlays.fullScreen,
      onToggleFullScreen: mediaActions.onToggleFullScreen,
      onTogglePlay: mediaActions.onTogglePlay,
      onStepForward: mediaActions.onStepForward,
      onStepBackward: mediaActions.onStepBackward,
      currentPtsUs: playback.currentPtsUs,
      durationUs: playback.durationUs,
      onSeek: mediaActions.onSeek,
      onHoverChanged: mediaActions.onSliderHover,
      markerUs: playback.markerUs,
      seekMinUs: playback.seekMinUs,
      seekMaxUs: playback.seekMaxUs,
    );
  }
}

class FullScreenControlsPanel extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewHandles handles;
  final MainWindowViewActions actions;

  const FullScreenControlsPanel({
    super.key,
    required this.model,
    required this.handles,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    final overlayActions = actions.overlays;
    return MouseRegion(
      onEnter: (_) => overlayActions.onFullScreenControlsHoverChanged(true),
      onExit: (_) => overlayActions.onFullScreenControlsHoverChanged(false),
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
                AnalysisOverlayStrip(
                  entries: model.media.tracks,
                  dataSource: model.media.analysisDataSource,
                  visible:
                      model.session.capabilities.canShowAnalysisOverlay &&
                      model.media.analysisOverlayEnabled &&
                      model.overlays.analysisOverlayControlsVisible,
                  onTypeChanged: actions.analysisOverlay.onTypeChanged,
                  onOpacityChanged: actions.analysisOverlay.onOpacityChanged,
                  onActivateOverlay: actions.analysisOverlay.onActivate,
                  onDeactivateOverlay: actions.analysisOverlay.onClose,
                  onClose: () {
                    actions.toolbar.onAnalysisOverlayPanelToggle();
                  },
                ),
                MainWindowMediaHeader(
                  model: model,
                  handles: handles,
                  actions: actions,
                ),
                MainWindowControlsBar(
                  model: model,
                  handles: handles,
                  actions: actions,
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
