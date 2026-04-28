import 'package:desktop_drop/desktop_drop.dart';
import 'package:flutter/material.dart';

import '../track_manager.dart';
import '../video_renderer_controller.dart';
import '../widgets/controls_bar.dart';
import '../widgets/loop_range_bar.dart';
import '../widgets/media_header.dart';
import '../widgets/timeline_area.dart';
import '../widgets/toolbar.dart';
import '../widgets/viewport_panel.dart';

class MainWindowView extends StatelessWidget {
  final bool dragging;
  final ValueChanged<List<String>> onFilesDropped;
  final VoidCallback onDragEntered;
  final VoidCallback onDragExited;

  final int viewMode;
  final ValueChanged<int> onViewModeChanged;
  final VoidCallback onAddMedia;
  final Future<void> Function() onAnalysis;
  final VoidCallback onProfiler;
  final VoidCallback onSettings;
  final bool viewModeEnabled;
  final bool analysisEnabled;

  final int? textureId;
  final int viewportState;
  final LayoutState layout;
  final ValueChanged<Offset> onPan;
  final ValueChanged<double> onSplit;
  final void Function(double scrollDelta, Offset localPos) onZoom;
  final void Function(bool panning, bool splitting) onPointerButton;
  final void Function(int width, int height) onResize;

  final TrackManager trackManager;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final ValueChanged<int> onRemoveTrack;

  final GlobalKey timelineSliderKey;
  final double timelineStartWidth;
  final ValueChanged<double> onZoomChanged;
  final bool isPlaying;
  final VoidCallback onTogglePlay;
  final VoidCallback onStepForward;
  final VoidCallback onStepBackward;
  final int currentPtsUs;
  final int durationUs;
  final ValueChanged<int> onSeek;
  final void Function(int hoverUs, bool hovering) onSliderHover;
  final List<int> markerUs;
  final int? seekMinUs;
  final int? seekMaxUs;

  final GlobalKey loopRangeBarKey;
  final bool loopRangeEnabled;
  final int loopStartUs;
  final int loopEndUs;
  final ValueChanged<bool> onLoopRangeEnabledChanged;
  final void Function(int startUs, int endUs) onLoopRangeChanged;
  final ValueChanged<LoopRangeHandle>? onLoopRangeChangeEnd;

  final void Function(int oldIndex, int newIndex) onReorder;
  final void Function(int slot, int offsetMs) onOffsetChanged;
  final Map<int, int> syncOffsets;
  final int hoverPtsUs;
  final bool sliderHovering;
  final double controlsWidth;
  final ValueChanged<double> onControlsWidthChanged;

  const MainWindowView({
    super.key,
    required this.dragging,
    required this.onFilesDropped,
    required this.onDragEntered,
    required this.onDragExited,
    required this.viewMode,
    required this.onViewModeChanged,
    required this.onAddMedia,
    required this.onAnalysis,
    required this.onProfiler,
    required this.onSettings,
    required this.viewModeEnabled,
    required this.analysisEnabled,
    required this.textureId,
    required this.viewportState,
    required this.layout,
    required this.onPan,
    required this.onSplit,
    required this.onZoom,
    required this.onPointerButton,
    required this.onResize,
    required this.trackManager,
    required this.onMediaSwapped,
    required this.onRemoveTrack,
    required this.timelineSliderKey,
    required this.timelineStartWidth,
    required this.onZoomChanged,
    required this.isPlaying,
    required this.onTogglePlay,
    required this.onStepForward,
    required this.onStepBackward,
    required this.currentPtsUs,
    required this.durationUs,
    required this.onSeek,
    required this.onSliderHover,
    required this.markerUs,
    required this.seekMinUs,
    required this.seekMaxUs,
    required this.loopRangeBarKey,
    required this.loopRangeEnabled,
    required this.loopStartUs,
    required this.loopEndUs,
    required this.onLoopRangeEnabledChanged,
    required this.onLoopRangeChanged,
    required this.onLoopRangeChangeEnd,
    required this.onReorder,
    required this.onOffsetChanged,
    required this.syncOffsets,
    required this.hoverPtsUs,
    required this.sliderHovering,
    required this.controlsWidth,
    required this.onControlsWidthChanged,
  });

  @override
  Widget build(BuildContext context) {
    return DropTarget(
      onDragEntered: (_) => onDragEntered(),
      onDragExited: (_) => onDragExited(),
      onDragDone: (details) {
        final paths = details.files
            .map((f) => f.path)
            .where((path) => path.isNotEmpty)
            .toList();
        if (paths.isNotEmpty) onFilesDropped(paths);
      },
      child: Scaffold(
        body: Stack(
          children: [
            Column(
              children: [
                AppToolBar(
                  viewMode: viewMode,
                  onViewModeChanged: onViewModeChanged,
                  onAddMedia: onAddMedia,
                  onAnalysis: onAnalysis,
                  onProfiler: onProfiler,
                  onSettings: onSettings,
                  viewModeEnabled: viewModeEnabled,
                  analysisEnabled: analysisEnabled,
                ),
                Expanded(
                  child: ViewportPanel(
                    textureId: textureId,
                    viewportState: viewportState,
                    layout: layout,
                    onPan: onPan,
                    onSplit: onSplit,
                    onZoom: onZoom,
                    onPointerButton: onPointerButton,
                    onResize: onResize,
                  ),
                ),
                if (trackManager.count > 0)
                  MediaHeaderBar(
                    entries: trackManager.entries,
                    onMediaSwapped: onMediaSwapped,
                    onRemoveClicked: (slotIndex) {
                      if (slotIndex < trackManager.count) {
                        onRemoveTrack(trackManager.entries[slotIndex].fileId);
                      }
                    },
                  ),
                if (trackManager.count > 0)
                  ControlsBar(
                    timelineKey: timelineSliderKey,
                    timelineStartWidth: timelineStartWidth,
                    zoomRatio: layout.zoomRatio,
                    onZoomChanged: onZoomChanged,
                    isPlaying: isPlaying,
                    onTogglePlay: onTogglePlay,
                    onStepForward: onStepForward,
                    onStepBackward: onStepBackward,
                    currentPtsUs: currentPtsUs,
                    durationUs: durationUs,
                    onSeek: onSeek,
                    onHoverChanged: onSliderHover,
                    markerUs: markerUs,
                    seekMinUs: seekMinUs,
                    seekMaxUs: seekMaxUs,
                  ),
                if (trackManager.count > 0)
                  LoopRangeBar(
                    key: loopRangeBarKey,
                    timelineStartWidth: timelineStartWidth,
                    enabled: loopRangeEnabled,
                    startUs: loopStartUs,
                    endUs: loopEndUs,
                    durationUs: durationUs,
                    onEnabledChanged: onLoopRangeEnabledChanged,
                    onRangeChanged: onLoopRangeChanged,
                    onRangeChangeEnd: onLoopRangeChangeEnd,
                  ),
                if (trackManager.count > 0)
                  Expanded(
                    flex: 0,
                    child: TimelineArea(
                      trackManager: trackManager,
                      currentPtsUs: currentPtsUs,
                      onRemoveTrack: onRemoveTrack,
                      onReorder: onReorder,
                      onOffsetChanged: onOffsetChanged,
                      syncOffsets: syncOffsets,
                      maxEffectiveDurationUs: durationUs,
                      hoverPtsUs: hoverPtsUs,
                      sliderHovering: sliderHovering,
                      controlsWidth: controlsWidth,
                      onControlsWidthChanged: onControlsWidthChanged,
                      markerPtsUs: markerUs,
                      loopRangeEnabled: loopRangeEnabled,
                      loopStartUs: loopStartUs,
                      loopEndUs: loopEndUs,
                    ),
                  ),
              ],
            ),
            if (dragging)
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
          ],
        ),
      ),
    );
  }
}
