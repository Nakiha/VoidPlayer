import 'package:flutter/material.dart';

import '../../performance/performance_health.dart';
import '../../platform/pointer_button_state_provider.dart';
import '../../widgets/app_feedback_host.dart';
import '../../widgets/axtree_region.dart';
import '../../widgets/media_header.dart';
import '../../widgets/toolbar.dart';
import '../../widgets/viewport_panel.dart';
import 'main_window_media_sections.dart';
import 'main_window_overlays.dart';
import 'main_window_view_model.dart';

class MainWindowScaffold extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;
  final PointerButtonStateProvider pointerButtonStateProvider;

  const MainWindowScaffold({
    super.key,
    required this.model,
    required this.actions,
    this.pointerButtonStateProvider = emptyPointerButtonStateProvider,
  });

  @override
  Widget build(BuildContext context) {
    final viewport = model.viewport;
    final media = model.media;
    final overlays = model.overlays;
    final toolbarActions = actions.toolbar;
    final viewportActions = actions.viewport;
    final overlayActions = actions.overlays;
    return Scaffold(
      body: Stack(
        children: [
          // Content-scoped overlays stay inside this host. Window overlays
          // below are later Stack siblings so their z-order stays explicit.
          MediaHeaderOverlayPanelHost(
            entries: media.tracks,
            dataSource: media.analysisDataSource,
            onOverlayActivate: actions.analysisOverlay.onActivate,
            onOverlayDeactivate: actions.analysisOverlay.onClose,
            onTypeChanged: actions.analysisOverlay.onTypeChanged,
            onOpacityChanged: actions.analysisOverlay.onOpacityChanged,
            child: Column(
              children: [
                if (!overlays.fullScreen)
                  AxTreeRegion(
                    label: 'Main toolbar',
                    child: AppToolBar(
                      viewMode: viewport.viewMode,
                      onViewModeChanged: toolbarActions.onViewModeChanged,
                      onOpenFile: toolbarActions.onOpenFile,
                      onOpenNetworkMedia: toolbarActions.onOpenNetworkMedia,
                      onOpenSshRemoteMedia: toolbarActions.onOpenSshRemoteMedia,
                      onMediaInfo: toolbarActions.onMediaInfo,
                      onAnalysis: toolbarActions.onAnalysis,
                      onProfiler: toolbarActions.onProfiler,
                      onSettings: toolbarActions.onSettings,
                      tracks: media.tracks,
                      analysisDataSource: media.analysisDataSource,
                      viewModeEnabled: viewport.viewModeEnabled,
                      nativePlaybackAvailable: media.nativePlaybackAvailable,
                      localFilePlaybackAvailable:
                          media.localFilePlaybackAvailable,
                      networkMediaAvailable: media.networkMediaAvailable,
                      sshRemoteMediaAvailable: media.sshRemoteMediaAvailable,
                      nativeFilePickerAvailable:
                          media.nativeFilePickerAvailable,
                      analysisEnabled: media.analysisEnabled,
                      mediaInfoActive: overlays.mediaInfoVisible,
                      profilerActive: overlays.profilerVisible,
                    ),
                  ),
                Expanded(
                  child: ViewportPanel(
                    key: viewport.viewportKey,
                    textureId: viewport.textureId,
                    viewportState: viewport.viewportState,
                    errorText: viewport.viewportState.errorText,
                    layout: viewport.layout,
                    onPan: viewportActions.onPan,
                    onSplit: viewportActions.onSplit,
                    onZoom: viewportActions.onZoom,
                    onPointerButton: viewportActions.onPointerButton,
                    onResize: viewportActions.onResize,
                    pointerButtonStateProvider: pointerButtonStateProvider,
                    nativePlaybackAvailable: media.nativePlaybackAvailable,
                  ),
                ),
                if (!overlays.fullScreen && media.tracks.isNotEmpty)
                  AxTreeRegion(
                    label: 'Playback timeline',
                    child: MediaTimelineSection(model: model, actions: actions),
                  ),
              ],
            ),
          ),
          if (overlays.fullScreen)
            FullScreenPointerCapture(
              onActivity: overlayActions.onFullScreenPointerActivity,
            ),
          if (overlays.fullScreen && media.tracks.isNotEmpty)
            FullScreenControlsOverlay(model: model, actions: actions),
          if (overlays.dragging) const DragDropLayer(),
          FloatingSidePanelsSlot(
            mediaInfoVisible: overlays.mediaInfoVisible,
            profilerVisible: overlays.profilerVisible,
            tracks: media.tracks,
            onCloseMediaInfo: overlayActions.onCloseMediaInfo,
            onCloseProfiler: overlayActions.onCloseProfiler,
          ),
          SettingsOverlaySlot(
            visible: overlays.settingsVisible,
            onClose: overlayActions.onCloseSettings,
            onViewportPixelSizeModeChanged:
                overlayActions.onViewportPixelSizeModeChanged,
            onPerformanceAlertPolicyChanged:
                overlayActions.onPerformanceAlertPolicyChanged,
          ),
          PerformanceHealthFeedbackMonitor(
            enabled:
                media.nativePlaybackAvailable &&
                media.tracks.isNotEmpty &&
                media.performanceAlertPolicy.enabled,
            trackCount: media.tracks.length,
            profilerVisible: overlays.profilerVisible,
            alertPolicy: media.performanceAlertPolicy,
            onOpenProfiler: toolbarActions.onProfiler,
          ),
          const AppFeedbackHost(),
        ],
      ),
    );
  }
}
