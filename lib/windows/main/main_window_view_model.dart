import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../../analysis/analysis_toolbar_data_source.dart';
import '../../preferences/playback_preferences.dart';
import '../../track_manager.dart';
import '../../video_renderer_controller.dart';
import '../../viewport/viewport_display_state.dart';
import '../../widgets/loop_range_bar.dart';
import 'main_window_state.dart';

class MainWindowViewModel {
  final MainWindowViewportVm viewport;
  final MainWindowMediaVm media;
  final MainWindowPlaybackVm playback;
  final MainWindowOverlayVm overlays;

  const MainWindowViewModel({
    required this.viewport,
    required this.media,
    required this.playback,
    required this.overlays,
  });
}

class MainWindowViewportVm {
  final int viewMode;
  final bool viewModeEnabled;
  final int? textureId;
  final ViewportDisplayState viewportState;
  final LayoutState layout;
  final GlobalKey viewportKey;

  const MainWindowViewportVm({
    required this.viewMode,
    required this.viewModeEnabled,
    required this.textureId,
    required this.viewportState,
    required this.layout,
    required this.viewportKey,
  });
}

class MainWindowMediaVm {
  final bool analysisEnabled;
  final List<TrackEntry> tracks;
  final Map<int, int> syncOffsets; // fileId -> offset in microseconds
  final int? audibleTrackFileId;
  final AnalysisToolbarDataSource analysisDataSource;

  const MainWindowMediaVm({
    required this.analysisEnabled,
    required this.tracks,
    required this.syncOffsets,
    required this.audibleTrackFileId,
    required this.analysisDataSource,
  });
}

class MainWindowPlaybackVm {
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
  final ValueListenable<TimelineHoverState> timelineHoverListenable;
  final double controlsWidth;

  const MainWindowPlaybackVm({
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
    required this.timelineHoverListenable,
    required this.controlsWidth,
  });
}

class MainWindowOverlayVm {
  final bool dragging;
  final bool mediaInfoVisible;
  final bool profilerVisible;
  final bool settingsVisible;
  final bool fullScreen;
  final bool fullScreenControlsVisible;

  const MainWindowOverlayVm({
    required this.dragging,
    required this.mediaInfoVisible,
    required this.profilerVisible,
    required this.settingsVisible,
    required this.fullScreen,
    required this.fullScreenControlsVisible,
  });
}

class MainWindowViewActions {
  final MainWindowDropActions drop;
  final MainWindowToolbarActions toolbar;
  final MainWindowViewportActions viewport;
  final MainWindowMediaTimelineActions mediaTimeline;
  final MainWindowOverlayActions overlays;

  const MainWindowViewActions({
    required this.drop,
    required this.toolbar,
    required this.viewport,
    required this.mediaTimeline,
    required this.overlays,
  });
}

class MainWindowDropActions {
  final ValueChanged<List<String>> filesDropped;
  final VoidCallback dragEntered;
  final VoidCallback dragExited;

  const MainWindowDropActions({
    required this.filesDropped,
    required this.dragEntered,
    required this.dragExited,
  });
}

class MainWindowToolbarActions {
  final ValueChanged<int> onViewModeChanged;
  final Future<void> Function() onOpenFile;
  final Future<void> Function(String url) onOpenNetworkMedia;
  final Future<void> Function(String remotePath) onOpenSshRemoteMedia;
  final VoidCallback onMediaInfo;
  final Future<void> Function() onAnalysis;
  final VoidCallback onProfiler;
  final VoidCallback onSettings;

  const MainWindowToolbarActions({
    required this.onViewModeChanged,
    required this.onOpenFile,
    required this.onOpenNetworkMedia,
    required this.onOpenSshRemoteMedia,
    required this.onMediaInfo,
    required this.onAnalysis,
    required this.onProfiler,
    required this.onSettings,
  });
}

class MainWindowViewportActions {
  final ValueChanged<Offset> onPan;
  final ValueChanged<double> onSplit;
  final void Function(double scrollDelta, Offset localPos) onZoom;
  final void Function(bool panning, bool splitting) onPointerButton;
  final void Function(int width, int height, double devicePixelRatio) onResize;

  const MainWindowViewportActions({
    required this.onPan,
    required this.onSplit,
    required this.onZoom,
    required this.onPointerButton,
    required this.onResize,
  });
}

class MainWindowMediaTimelineActions {
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final ValueChanged<int> onRemoveTrack;
  final ValueChanged<double> onZoomChanged;
  final VoidCallback onToggleFullScreen;
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

  const MainWindowMediaTimelineActions({
    required this.onMediaSwapped,
    required this.onRemoveTrack,
    required this.onZoomChanged,
    required this.onToggleFullScreen,
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

class MainWindowOverlayActions {
  final VoidCallback onCloseMediaInfo;
  final VoidCallback onCloseProfiler;
  final VoidCallback onCloseSettings;
  final ValueChanged<ViewportPixelSizeMode> onViewportPixelSizeModeChanged;
  final VoidCallback onFullScreenPointerActivity;
  final void Function(bool hovering) onFullScreenControlsHoverChanged;

  const MainWindowOverlayActions({
    required this.onCloseMediaInfo,
    required this.onCloseProfiler,
    required this.onCloseSettings,
    required this.onViewportPixelSizeModeChanged,
    required this.onFullScreenPointerActivity,
    required this.onFullScreenControlsHoverChanged,
  });
}
