import 'package:flutter/material.dart';

import '../../widgets/toolbar.dart';
import '../../widgets/viewport_panel.dart';
import '../win32_pointer_button_state_provider.dart';
import 'main_window_media_sections.dart';
import 'main_window_overlays.dart';
import 'main_window_view_model.dart';

class MainWindowScaffold extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const MainWindowScaffold({
    super.key,
    required this.model,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    final viewport = model.viewport;
    final media = model.media;
    final overlays = model.overlays;
    return Scaffold(
      body: Stack(
        children: [
          Column(
            children: [
              if (!overlays.fullScreen)
                AppToolBar(
                  viewMode: viewport.viewMode,
                  onViewModeChanged: actions.onViewModeChanged,
                  onAddMedia: actions.onAddMedia,
                  onAnalysis: actions.onAnalysis,
                  onProfiler: actions.onProfiler,
                  onSettings: actions.onSettings,
                  tracks: media.tracks,
                  viewModeEnabled: viewport.viewModeEnabled,
                  analysisEnabled: media.analysisEnabled,
                ),
              Expanded(
                child: ViewportPanel(
                  key: viewport.viewportKey,
                  textureId: viewport.textureId,
                  viewportState: viewport.viewportState,
                  errorText: viewport.viewportState.errorText,
                  layout: viewport.layout,
                  onPan: actions.onPan,
                  onSplit: actions.onSplit,
                  onZoom: actions.onZoom,
                  onPointerButton: actions.onPointerButton,
                  onResize: actions.onResize,
                  pointerButtonStateProvider:
                      const Win32PointerButtonStateProvider(),
                ),
              ),
              if (!overlays.fullScreen && media.tracks.isNotEmpty)
                MediaTimelineSection(model: model, actions: actions),
            ],
          ),
          if (overlays.fullScreen)
            FullScreenPointerCapture(
              onActivity: actions.onFullScreenPointerActivity,
            ),
          if (overlays.fullScreen && media.tracks.isNotEmpty)
            FullScreenControlsOverlay(model: model, actions: actions),
          if (overlays.dragging) const DragDropLayer(),
          ProfilerOverlaySlot(
            visible: overlays.profilerVisible,
            onClose: actions.onCloseProfiler,
          ),
          SettingsOverlaySlot(
            visible: overlays.settingsVisible,
            onClose: actions.onCloseSettings,
          ),
        ],
      ),
    );
  }
}
