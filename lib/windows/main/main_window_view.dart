import 'package:desktop_drop/desktop_drop.dart';
import 'package:flutter/material.dart';

import '../../l10n/app_localizations.dart';
import '../../track_manager.dart';
import '../../video_renderer_controller.dart';
import '../../widgets/controls_bar.dart';
import '../../widgets/loop_range_bar.dart';
import '../../widgets/media_header.dart';
import '../../widgets/timeline_area.dart';
import '../../widgets/toolbar.dart';
import '../../widgets/viewport_panel.dart';
import '../settings_window.dart';
import '../stats_window.dart';

class MainWindowViewModel {
  final bool dragging;
  final int viewMode;
  final bool viewModeEnabled;
  final bool analysisEnabled;
  final int? textureId;
  final int viewportState;
  final LayoutState layout;
  final List<TrackEntry> tracks;
  final GlobalKey timelineSliderKey;
  final double timelineStartWidth;
  final bool isPlaying;
  final int currentPtsUs;
  final int durationUs;
  final List<int> markerUs;
  final int? seekMinUs;
  final int? seekMaxUs;
  final GlobalKey loopRangeBarKey;
  final bool loopRangeEnabled;
  final int loopStartUs;
  final int loopEndUs;
  final Map<int, int> syncOffsets;
  final int hoverPtsUs;
  final bool sliderHovering;
  final double controlsWidth;
  final bool profilerVisible;
  final bool settingsVisible;

  const MainWindowViewModel({
    required this.dragging,
    required this.viewMode,
    required this.viewModeEnabled,
    required this.analysisEnabled,
    required this.textureId,
    required this.viewportState,
    required this.layout,
    required this.tracks,
    required this.timelineSliderKey,
    required this.timelineStartWidth,
    required this.isPlaying,
    required this.currentPtsUs,
    required this.durationUs,
    required this.markerUs,
    required this.seekMinUs,
    required this.seekMaxUs,
    required this.loopRangeBarKey,
    required this.loopRangeEnabled,
    required this.loopStartUs,
    required this.loopEndUs,
    required this.syncOffsets,
    required this.hoverPtsUs,
    required this.sliderHovering,
    required this.controlsWidth,
    required this.profilerVisible,
    required this.settingsVisible,
  });
}

class MainWindowViewActions {
  final ValueChanged<List<String>> onFilesDropped;
  final VoidCallback onDragEntered;
  final VoidCallback onDragExited;
  final ValueChanged<int> onViewModeChanged;
  final VoidCallback onAddMedia;
  final Future<void> Function() onAnalysis;
  final VoidCallback onProfiler;
  final VoidCallback onCloseProfiler;
  final VoidCallback onSettings;
  final VoidCallback onCloseSettings;
  final ValueChanged<Offset> onPan;
  final ValueChanged<double> onSplit;
  final void Function(double scrollDelta, Offset localPos) onZoom;
  final void Function(bool panning, bool splitting) onPointerButton;
  final void Function(int width, int height) onResize;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final ValueChanged<int> onRemoveTrack;
  final ValueChanged<double> onZoomChanged;
  final VoidCallback onTogglePlay;
  final VoidCallback onStepForward;
  final VoidCallback onStepBackward;
  final ValueChanged<int> onSeek;
  final void Function(int hoverUs, bool hovering) onSliderHover;
  final ValueChanged<bool> onLoopRangeEnabledChanged;
  final void Function(int startUs, int endUs) onLoopRangeChanged;
  final ValueChanged<LoopRangeHandle>? onLoopRangeChangeEnd;
  final void Function(int oldIndex, int newIndex) onReorder;
  final void Function(int slot, int offsetMs) onOffsetChanged;
  final ValueChanged<double> onControlsWidthChanged;

  const MainWindowViewActions({
    required this.onFilesDropped,
    required this.onDragEntered,
    required this.onDragExited,
    required this.onViewModeChanged,
    required this.onAddMedia,
    required this.onAnalysis,
    required this.onProfiler,
    required this.onCloseProfiler,
    required this.onSettings,
    required this.onCloseSettings,
    required this.onPan,
    required this.onSplit,
    required this.onZoom,
    required this.onPointerButton,
    required this.onResize,
    required this.onMediaSwapped,
    required this.onRemoveTrack,
    required this.onZoomChanged,
    required this.onTogglePlay,
    required this.onStepForward,
    required this.onStepBackward,
    required this.onSeek,
    required this.onSliderHover,
    required this.onLoopRangeEnabledChanged,
    required this.onLoopRangeChanged,
    required this.onLoopRangeChangeEnd,
    required this.onReorder,
    required this.onOffsetChanged,
    required this.onControlsWidthChanged,
  });
}

class MainWindowView extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const MainWindowView({super.key, required this.model, required this.actions});

  @override
  Widget build(BuildContext context) {
    return DropTarget(
      onDragEntered: (_) => actions.onDragEntered(),
      onDragExited: (_) => actions.onDragExited(),
      onDragDone: (details) {
        final paths = details.files
            .map((f) => f.path)
            .where((path) => path.isNotEmpty)
            .toList();
        if (paths.isNotEmpty) actions.onFilesDropped(paths);
      },
      child: Scaffold(
        body: Stack(
          children: [
            Column(
              children: [
                AppToolBar(
                  viewMode: model.viewMode,
                  onViewModeChanged: actions.onViewModeChanged,
                  onAddMedia: actions.onAddMedia,
                  onAnalysis: actions.onAnalysis,
                  onProfiler: actions.onProfiler,
                  onSettings: actions.onSettings,
                  viewModeEnabled: model.viewModeEnabled,
                  analysisEnabled: model.analysisEnabled,
                ),
                Expanded(
                  child: ViewportPanel(
                    textureId: model.textureId,
                    viewportState: model.viewportState,
                    layout: model.layout,
                    onPan: actions.onPan,
                    onSplit: actions.onSplit,
                    onZoom: actions.onZoom,
                    onPointerButton: actions.onPointerButton,
                    onResize: actions.onResize,
                  ),
                ),
                if (model.tracks.isNotEmpty)
                  MediaHeaderBar(
                    entries: model.tracks,
                    onMediaSwapped: actions.onMediaSwapped,
                    onRemoveClicked: (slotIndex) {
                      if (slotIndex < model.tracks.length) {
                        actions.onRemoveTrack(model.tracks[slotIndex].fileId);
                      }
                    },
                  ),
                if (model.tracks.isNotEmpty)
                  ControlsBar(
                    timelineKey: model.timelineSliderKey,
                    timelineStartWidth: model.timelineStartWidth,
                    zoomRatio: model.layout.zoomRatio,
                    onZoomChanged: actions.onZoomChanged,
                    isPlaying: model.isPlaying,
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
                  ),
                if (model.tracks.isNotEmpty)
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
                if (model.tracks.isNotEmpty)
                  Expanded(
                    flex: 0,
                    child: TimelineArea(
                      entries: model.tracks,
                      currentPtsUs: model.currentPtsUs,
                      onRemoveTrack: actions.onRemoveTrack,
                      onReorder: actions.onReorder,
                      onOffsetChanged: actions.onOffsetChanged,
                      syncOffsets: model.syncOffsets,
                      maxEffectiveDurationUs: model.durationUs,
                      hoverPtsUs: model.hoverPtsUs,
                      sliderHovering: model.sliderHovering,
                      controlsWidth: model.controlsWidth,
                      onControlsWidthChanged: actions.onControlsWidthChanged,
                      markerPtsUs: model.markerUs,
                      loopRangeEnabled: model.loopRangeEnabled,
                      loopStartUs: model.loopStartUs,
                      loopEndUs: model.loopEndUs,
                    ),
                  ),
              ],
            ),
            if (model.dragging)
              Positioned.fill(
                child: IgnorePointer(
                  child: Container(
                    decoration: BoxDecoration(
                      border: Border.all(
                        color: Theme.of(
                          context,
                        ).colorScheme.primary.withValues(alpha: 0.5),
                        width: 3,
                      ),
                      color: Theme.of(
                        context,
                      ).colorScheme.primary.withValues(alpha: 0.08),
                    ),
                  ),
                ),
              ),
            if (model.profilerVisible)
              Positioned(
                top: 48,
                right: 12,
                bottom: 12,
                left: 12,
                child: Align(
                  alignment: Alignment.centerRight,
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(maxWidth: 640),
                    child: _ProfilerOverlay(onClose: actions.onCloseProfiler),
                  ),
                ),
              ),
            if (model.settingsVisible)
              Positioned.fill(
                child: _ModalScrim(
                  onDismiss: actions.onCloseSettings,
                  child: _SettingsDialog(onClose: actions.onCloseSettings),
                ),
              ),
          ],
        ),
      ),
    );
  }
}

class _ModalScrim extends StatelessWidget {
  final VoidCallback onDismiss;
  final Widget child;

  const _ModalScrim({required this.onDismiss, required this.child});

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.black.withValues(alpha: 0.32),
      child: Stack(
        children: [
          Positioned.fill(
            child: GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTap: onDismiss,
            ),
          ),
          Center(child: child),
        ],
      ),
    );
  }
}

class _ProfilerOverlay extends StatelessWidget {
  final VoidCallback onClose;

  const _ProfilerOverlay({required this.onClose});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Material(
      elevation: 12,
      color: theme.colorScheme.surface,
      borderRadius: BorderRadius.circular(8),
      clipBehavior: Clip.antiAlias,
      child: Column(
        children: [
          SizedBox(
            height: 40,
            child: Row(
              children: [
                const SizedBox(width: 12),
                Icon(Icons.speed, size: 18, color: theme.colorScheme.primary),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    AppLocalizations.of(context)!.performanceMonitor,
                    style: theme.textTheme.titleSmall,
                  ),
                ),
                IconButton(
                  onPressed: onClose,
                  icon: const Icon(Icons.close, size: 18),
                  tooltip: MaterialLocalizations.of(context).closeButtonTooltip,
                ),
              ],
            ),
          ),
          const Divider(height: 1),
          const Expanded(child: StatsPage()),
        ],
      ),
    );
  }
}

class _SettingsDialog extends StatelessWidget {
  final VoidCallback onClose;

  const _SettingsDialog({required this.onClose});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return ConstrainedBox(
      constraints: const BoxConstraints(
        maxWidth: 760,
        maxHeight: 560,
        minWidth: 520,
      ),
      child: Material(
        elevation: 16,
        color: theme.colorScheme.surface,
        borderRadius: BorderRadius.circular(8),
        clipBehavior: Clip.antiAlias,
        child: Column(
          children: [
            SizedBox(
              height: 44,
              child: Row(
                children: [
                  const SizedBox(width: 12),
                  Icon(
                    Icons.settings,
                    size: 18,
                    color: theme.colorScheme.primary,
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      AppLocalizations.of(context)!.settings,
                      style: theme.textTheme.titleSmall,
                    ),
                  ),
                  IconButton(
                    onPressed: onClose,
                    icon: const Icon(Icons.close, size: 18),
                    tooltip: MaterialLocalizations.of(
                      context,
                    ).closeButtonTooltip,
                  ),
                ],
              ),
            ),
            const Divider(height: 1),
            const Expanded(child: SettingsPage()),
          ],
        ),
      ),
    );
  }
}
