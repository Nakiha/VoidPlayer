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
    return Scaffold(
      body: Stack(
        children: [
          Column(
            children: [
              if (!model.fullScreen)
                AppToolBar(
                  viewMode: model.viewMode,
                  onViewModeChanged: actions.onViewModeChanged,
                  onAddMedia: actions.onAddMedia,
                  onAnalysis: actions.onAnalysis,
                  onProfiler: actions.onProfiler,
                  onSettings: actions.onSettings,
                  tracks: model.tracks,
                  viewModeEnabled: model.viewModeEnabled,
                  analysisEnabled: model.analysisEnabled,
                ),
              Expanded(
                child: ViewportPanel(
                  key: model.viewportKey,
                  textureId: model.textureId,
                  viewportState: model.viewportState,
                  errorText: model.viewportState.errorText,
                  layout: model.layout,
                  onPan: actions.onPan,
                  onSplit: actions.onSplit,
                  onZoom: actions.onZoom,
                  onPointerButton: actions.onPointerButton,
                  onResize: actions.onResize,
                  pointerButtonStateProvider:
                      const Win32PointerButtonStateProvider(),
                ),
              ),
              if (!model.fullScreen && model.tracks.isNotEmpty)
                MediaTimelineSection(model: model, actions: actions),
            ],
          ),
          if (model.fullScreen)
            FullScreenPointerCapture(
              onActivity: actions.onFullScreenPointerActivity,
            ),
          if (model.fullScreen && model.tracks.isNotEmpty)
            FullScreenControlsOverlay(model: model, actions: actions),
          if (model.dragging) const DragDropLayer(),
          ProfilerOverlaySlot(
            visible: model.profilerVisible,
            onClose: actions.onCloseProfiler,
          ),
          SettingsOverlaySlot(
            visible: model.settingsVisible,
            onClose: actions.onCloseSettings,
          ),
        ],
      ),
    );
  }
}
