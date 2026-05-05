import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../../track_manager.dart';
import '../../video_renderer_controller.dart';
import '../../viewport/viewport_display_state.dart';
import '../../widgets/loop_range_bar.dart';
import 'main_window_state.dart';

class MainWindowViewModel {
  final bool dragging;
  final int viewMode;
  final bool viewModeEnabled;
  final bool analysisEnabled;
  final int? textureId;
  final ViewportDisplayState viewportState;
  final LayoutState layout;
  final List<TrackEntry> tracks;
  final GlobalKey viewportKey;
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
  final Map<int, int> syncOffsets; // fileId -> offset in microseconds
  final ValueListenable<TimelineHoverState> timelineHoverListenable;
  final double controlsWidth;
  final bool profilerVisible;
  final bool settingsVisible;
  final bool fullScreen;
  final bool fullScreenControlsVisible;
  final int? audibleTrackFileId;

  const MainWindowViewModel({
    required this.dragging,
    required this.viewMode,
    required this.viewModeEnabled,
    required this.analysisEnabled,
    required this.textureId,
    required this.viewportState,
    required this.layout,
    required this.tracks,
    required this.viewportKey,
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
    required this.timelineHoverListenable,
    required this.controlsWidth,
    required this.profilerVisible,
    required this.settingsVisible,
    required this.fullScreen,
    required this.fullScreenControlsVisible,
    required this.audibleTrackFileId,
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
  final void Function(int width, int height, double devicePixelRatio) onResize;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final ValueChanged<int> onRemoveTrack;
  final ValueChanged<double> onZoomChanged;
  final VoidCallback onToggleFullScreen;
  final VoidCallback onFullScreenPointerActivity;
  final void Function(bool hovering) onFullScreenControlsHoverChanged;
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
  final ValueChanged<int> onToggleTrackAudio;
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
    required this.onToggleFullScreen,
    required this.onFullScreenPointerActivity,
    required this.onFullScreenControlsHoverChanged,
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
    required this.onToggleTrackAudio,
    required this.onControlsWidthChanged,
  });
}
