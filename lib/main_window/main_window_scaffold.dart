import 'package:flutter/material.dart';

import '../app_log.dart';
import '../native_compositor_flags.dart';
import '../performance/performance_health.dart';
import '../platform/platform_capabilities.dart';
import '../platform/pointer_button_state_provider.dart';
import '../viewport/viewport_display_state.dart';
import '../widgets/app_feedback_host.dart';
import '../widgets/axtree_region.dart';
import '../widgets/resizable_divider.dart';
import '../widgets/toolbar.dart';
import '../widgets/viewport_panel.dart';
import 'main_window_deck.dart';
import 'main_window_inspector.dart';
import 'main_window_list_sidebar.dart';
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
    final inspectorVisible = !model.selection.isEmpty;
    final leftPanelVisible = overlays.marksSidebarVisible;
    final rightPanelVisible = inspectorVisible;
    final nativeCompositor = NativeCompositorFlags.nativeCompositor;
    final nativeCompositorViewportActive =
        nativeCompositor &&
        viewport.nativeCompositorActive &&
        viewport.viewModeEnabled &&
        viewport.viewportState.status == ViewportDisplayStatus.active;
    if (overlays.settingsVisible ||
        overlays.mediaInfoVisible ||
        overlays.profilerVisible ||
        leftPanelVisible ||
        rightPanelVisible) {
      log.fine(
        '[NativeCompositorDebug] scaffold overlay build '
        'nativeHole=$nativeCompositorViewportActive '
        'settings=${overlays.settingsVisible} '
        'mediaInfo=${overlays.mediaInfoVisible} '
        'profiler=${overlays.profilerVisible} '
        'leftPanel=$leftPanelVisible '
        'rightPanel=$rightPanelVisible',
      );
    }
    final shellBackgroundColor = Theme.of(context).scaffoldBackgroundColor;
    return Scaffold(
      backgroundColor: nativeCompositorViewportActive
          ? Colors.transparent
          : null,
      body: Listener(
        behavior: HitTestBehavior.translucent,
        onPointerDown: (event) {
          log.fine(
            '[NativeCompositorDebug] root pointerDown '
            'global=${event.position} local=${event.localPosition} '
            'buttons=${event.buttons} tracks=${media.tracks.length} '
            'nativeHole=$nativeCompositorViewportActive '
            'rightPanel=$rightPanelVisible '
            'settings=${overlays.settingsVisible} '
            'dragging=${overlays.dragging}',
          );
        },
        child: Stack(
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
                        onOpenSshRemoteMedia:
                            toolbarActions.onOpenSshRemoteMedia,
                        onMediaInfo: toolbarActions.onMediaInfo,
                        onAnalysis: toolbarActions.onAnalysis,
                        onProfiler: toolbarActions.onProfiler,
                        onSettings: toolbarActions.onSettings,
                        onMarksSidebarToggle:
                            toolbarActions.onMarksSidebarToggle,
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
                        analysisDisabledTooltip: null,
                        canAddTrack: capabilities.canAddTrack,
                        canOpenLocalMedia: capabilities.canOpenLocalMedia,
                        canOpenNetworkMedia: capabilities.canOpenNetworkMedia,
                        canOpenSshMedia: capabilities.canOpenSshMedia,
                        canOpenMediaInfo: capabilities.canOpenMediaInfo,
                        canOpenProfiler: capabilities.canOpenProfiler,
                        canRunAnalysis: media.tracks.isNotEmpty,
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
                          if (leftPanelVisible)
                            _NativeCompositorOpaqueRegion(
                              enabled: nativeCompositorViewportActive,
                              color: shellBackgroundColor,
                              child: MainWindowListSidebar(
                                width: overlays.marksSidebarWidth,
                                model: model,
                                actions: actions,
                                onClose: overlayActions.onCloseMarksSidebar,
                              ),
                            ),
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
                                    onQuickMarkInteraction:
                                        viewportActions.onQuickMarkInteraction,
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
                                      child: Column(
                                        mainAxisSize: MainAxisSize.min,
                                        children: [
                                          PinnedPlaybackChrome(
                                            model: model,
                                            handles: handles,
                                            actions: actions,
                                          ),
                                          MainWindowDeck(
                                            model: model,
                                            handles: handles,
                                            actions: actions,
                                          ),
                                        ],
                                      ),
                                    ),
                                  ),
                              ],
                            ),
                          ),
                          if (rightPanelVisible)
                            _NativeCompositorOpaqueRegion(
                              enabled: nativeCompositorViewportActive,
                              color: shellBackgroundColor,
                              child: MainWindowInspector(
                                width: overlays.marksSidebarWidth,
                                selection: model.selection,
                                marks: model.marks,
                                media: model.media,
                                markActions: actions.marks,
                                onClose: () =>
                                    overlayActions.onCloseInspector?.call(),
                              ),
                            ),
                        ],
                      ),
                      if (leftPanelVisible)
                        _SidebarResizeHandle(
                          width: overlays.marksSidebarWidth,
                          fromLeft: true,
                          onWidthChanged:
                              overlayActions.onMarksSidebarWidthChanged,
                        ),
                      if (rightPanelVisible)
                        _SidebarResizeHandle(
                          width: overlays.marksSidebarWidth,
                          fromLeft: false,
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
      ),
    );
  }
}

class _SidebarResizeHandle extends StatelessWidget {
  final double width;
  final bool fromLeft;
  final ValueChanged<double> onWidthChanged;

  const _SidebarResizeHandle({
    required this.width,
    required this.fromLeft,
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
            left: fromLeft ? width : null,
            right: fromLeft ? null : width,
            bottom: 0,
            width: 1,
            child: ExcludeSemantics(
              child: ColoredBox(color: colorScheme.outlineVariant),
            ),
          ),
          Positioned(
            top: 0,
            left: fromLeft ? width - kMarksSidebarResizeHandleWidth / 2 : null,
            right: fromLeft ? null : width - kMarksSidebarResizeHandleWidth / 2,
            bottom: 0,
            width: kMarksSidebarResizeHandleWidth,
            child: ExcludeSemantics(
              child: ResizableVerticalDivider(
                color: colorScheme.outlineVariant,
                value: width,
                minValue: kMinMarksSidebarWidth,
                maxValue: kMaxMarksSidebarWidth,
                deltaScale: fromLeft ? 1 : -1,
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
