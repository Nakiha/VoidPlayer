import 'package:flutter/material.dart';

import '../native_compositor_flags.dart';
import '../performance/performance_health.dart';
import '../platform/platform_capabilities.dart';
import '../platform/pointer_button_state_provider.dart';
import '../viewport/viewport_display_state.dart';
import '../widgets/app_feedback_host.dart';
import '../widgets/axtree_region.dart';
import '../widgets/quick_mark_sidebar.dart';
import '../widgets/resizable_divider.dart';
import '../widgets/toolbar.dart';
import '../widgets/viewport_panel.dart';
import 'main_window_media_sections.dart';
import 'main_window_overlays.dart';
import 'main_window_state.dart';
import 'main_window_view_handles.dart';
import 'main_window_view_model.dart';

class MainWindowScaffold extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewHandles handles;
  final MainWindowViewActions actions;
  final PointerButtonStateProvider pointerButtonStateProvider;

  const MainWindowScaffold({
    super.key,
    required this.model,
    required this.handles,
    required this.actions,
    this.pointerButtonStateProvider = emptyPointerButtonStateProvider,
  });

  @override
  Widget build(BuildContext context) {
    final viewport = model.viewport;
    final media = model.media;
    final overlays = model.overlays;
    final capabilities = model.session.capabilities;
    final toolbarActions = actions.toolbar;
    final viewportActions = actions.viewport;
    final overlayActions = actions.overlays;
    final nativeCompositor = NativeCompositorFlags.nativeCompositor;
    final nativeCompositorViewportActive =
        nativeCompositor &&
        viewport.textureId != null &&
        viewport.viewportState.status == ViewportDisplayStatus.active;
    final shellBackgroundColor = Theme.of(context).scaffoldBackgroundColor;
    return Scaffold(
      backgroundColor: nativeCompositorViewportActive
          ? Colors.transparent
          : null,
      body: Stack(
        children: [
          Column(
            children: [
              if (!overlays.fullScreen)
                AxTreeRegion(
                  label: 'Main toolbar',
                  child: _NativeCompositorOpaqueRegion(
                    enabled: nativeCompositorViewportActive,
                    color: shellBackgroundColor,
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
                      onMarksSidebarToggle: toolbarActions.onMarksSidebarToggle,
                      tracks: media.tracks,
                      analysisDataSource: media.analysisDataSource,
                      viewModeEnabled:
                          capabilities.canChangeViewMode &&
                          viewport.viewModeEnabled,
                      nativePlaybackAvailable: media.nativePlaybackAvailable,
                      localFilePlaybackAvailable:
                          media.localFilePlaybackAvailable,
                      networkMediaAvailable: media.networkMediaAvailable,
                      sshRemoteMediaAvailable: media.sshRemoteMediaAvailable,
                      nativeFilePickerAvailable:
                          media.nativeFilePickerAvailable,
                      addMediaDisabledTooltip: firstCapabilityUserMessage([
                        media.localFilePlaybackCapability,
                        media.nativeFilePickerCapability,
                        media.networkMediaPlaybackCapability,
                        media.sshRemoteMediaPlaybackCapability,
                      ]),
                      analysisDisabledTooltip: firstCapabilityUserMessage([
                        media.externalAnalysisWindowsCapability,
                        media.analysisOverlaysCapability,
                      ]),
                      canAddTrack: capabilities.canAddTrack,
                      canOpenLocalMedia: capabilities.canOpenLocalMedia,
                      canOpenNetworkMedia: capabilities.canOpenNetworkMedia,
                      canOpenSshMedia: capabilities.canOpenSshMedia,
                      canOpenMediaInfo: capabilities.canOpenMediaInfo,
                      canOpenProfiler: capabilities.canOpenProfiler,
                      canRunAnalysis: capabilities.canRunAnalysis,
                      analysisEnabled: media.analysisEnabled,
                      mediaInfoActive: overlays.mediaInfoVisible,
                      profilerActive: overlays.profilerVisible,
                      marksSidebarActive: overlays.marksSidebarVisible,
                    ),
                  ),
                ),
              Expanded(
                child: Stack(
                  children: [
                    Row(
                      children: [
                        Expanded(
                          child: Column(
                            children: [
                              Expanded(
                                child: ViewportPanel(
                                  key: handles.viewportKey,
                                  textureId: viewport.textureId,
                                  viewportState: viewport.viewportState,
                                  errorText: viewport.viewportState.errorText,
                                  layout: viewport.layout,
                                  onPan: viewportActions.onPan,
                                  onSplit: viewportActions.onSplit,
                                  onZoom: viewportActions.onZoom,
                                  onPointerButton:
                                      viewportActions.onPointerButton,
                                  onResize: viewportActions.onResize,
                                  onNativeCompositorViewportRect:
                                      viewportActions
                                          .onNativeCompositorViewportRect,
                                  trackGeometry: viewport.tracks,
                                  quickMarks: viewport.quickMarks,
                                  quickMarkDraft: viewport.quickMarkDraft,
                                  selectedQuickMarkId:
                                      viewport.selectedQuickMarkId,
                                  onQuickMarkStart:
                                      viewportActions.onQuickMarkStart,
                                  onQuickMarkUpdate:
                                      viewportActions.onQuickMarkUpdate,
                                  onQuickMarkEnd:
                                      viewportActions.onQuickMarkEnd,
                                  onQuickMarkCancel:
                                      viewportActions.onQuickMarkCancel,
                                  onQuickMarkSelect:
                                      viewportActions.onQuickMarkSelect,
                                  onQuickMarkChanged:
                                      viewportActions.onQuickMarkChanged,
                                  onQuickMarkDeleted:
                                      viewportActions.onQuickMarkDeleted,
                                  onQuickMarkFocus:
                                      viewportActions.onQuickMarkFocus,
                                  pointerButtonStateProvider:
                                      pointerButtonStateProvider,
                                  nativePlaybackAvailable:
                                      media.nativePlaybackAvailable,
                                  nativeCompositorHole:
                                      nativeCompositorViewportActive,
                                ),
                              ),
                              if (!overlays.fullScreen &&
                                  media.tracks.isNotEmpty)
                                AxTreeRegion(
                                  label: 'Playback timeline',
                                  child: _NativeCompositorOpaqueRegion(
                                    enabled: nativeCompositorViewportActive,
                                    color: shellBackgroundColor,
                                    child: MediaTimelineSection(
                                      model: model,
                                      handles: handles,
                                      actions: actions,
                                    ),
                                  ),
                                ),
                            ],
                          ),
                        ),
                        if (overlays.marksSidebarVisible)
                          _NativeCompositorOpaqueRegion(
                            enabled: nativeCompositorViewportActive,
                            color: shellBackgroundColor,
                            child: QuickMarkSidebar(
                              width: overlays.marksSidebarWidth,
                              marks: model.marks,
                              actions: actions.marks,
                              onClose: overlayActions.onCloseMarksSidebar,
                            ),
                          ),
                      ],
                    ),
                    if (overlays.marksSidebarVisible)
                      _MarksSidebarResizeHandle(
                        width: overlays.marksSidebarWidth,
                        onWidthChanged:
                            overlayActions.onMarksSidebarWidthChanged,
                      ),
                  ],
                ),
              ),
            ],
          ),
          if (overlays.fullScreen)
            FullScreenPointerCapture(
              onActivity: overlayActions.onFullScreenPointerActivity,
            ),
          if (overlays.fullScreen && media.tracks.isNotEmpty)
            FullScreenControlsOverlay(
              model: model,
              handles: handles,
              actions: actions,
            ),
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

class _MarksSidebarResizeHandle extends StatelessWidget {
  final double width;
  final ValueChanged<double> onWidthChanged;

  const _MarksSidebarResizeHandle({
    required this.width,
    required this.onWidthChanged,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Positioned.fill(
      child: Stack(
        children: [
          Positioned(
            top: 0,
            right: width,
            bottom: 0,
            width: 1,
            child: ExcludeSemantics(
              child: ColoredBox(color: colorScheme.outlineVariant),
            ),
          ),
          Positioned(
            top: 0,
            right: width - kMarksSidebarResizeHandleWidth / 2,
            bottom: 0,
            width: kMarksSidebarResizeHandleWidth,
            child: ExcludeSemantics(
              child: ResizableVerticalDivider(
                color: colorScheme.outlineVariant,
                value: width,
                minValue: kMinMarksSidebarWidth,
                maxValue: kMaxMarksSidebarWidth,
                deltaScale: -1,
                onValueChanged: onWidthChanged,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _NativeCompositorOpaqueRegion extends StatelessWidget {
  final bool enabled;
  final Color color;
  final Widget child;

  const _NativeCompositorOpaqueRegion({
    required this.enabled,
    required this.color,
    required this.child,
  });

  @override
  Widget build(BuildContext context) {
    if (!enabled) return child;
    return ColoredBox(color: color, child: child);
  }
}
