import 'package:flutter/material.dart';

import '../../analysis/analysis_overlay.dart';
import '../../analysis/analysis_toolbar_data_source.dart';
import '../../marks/quick_mark.dart';
import '../../marks/quick_mark_thumbnail.dart';
import '../../platform/platform_capabilities.dart';
import '../../preferences/playback_preferences.dart';
import '../../session/playback_session.dart';
import '../../track_manager.dart';
import '../../video_renderer_controller.dart';
import '../../viewport/display_geometry.dart';
import '../../viewport/viewport_display_state.dart';
import '../../widgets/loop_range_bar.dart';

class MainWindowViewModel {
  final MainWindowSessionVm session;
  final MainWindowViewportVm viewport;
  final MainWindowMarksVm marks;
  final MainWindowMediaVm media;
  final MainWindowPlaybackVm playback;
  final MainWindowOverlayVm overlays;

  const MainWindowViewModel({
    required this.session,
    required this.viewport,
    required this.marks,
    required this.media,
    required this.playback,
    required this.overlays,
  });
}

class MainWindowSessionVm {
  final PlaybackSessionKind kind;
  final SessionCapabilities capabilities;
  final SessionRangeConstraint rangeConstraint;

  const MainWindowSessionVm({
    required this.kind,
    required this.capabilities,
    required this.rangeConstraint,
  });

  factory MainWindowSessionVm.fromSession(PlaybackSession session) {
    return MainWindowSessionVm(
      kind: session.kind,
      capabilities: session.capabilities,
      rangeConstraint: session.rangeConstraint,
    );
  }
}

class MainWindowViewportVm {
  final int viewMode;
  final bool viewModeEnabled;
  final int? textureId;
  final ViewportDisplayState viewportState;
  final LayoutState layout;
  final List<DisplayTrackGeometry> tracks;
  final List<QuickMark> quickMarks;
  final QuickMark? quickMarkDraft;
  final int? selectedQuickMarkId;

  const MainWindowViewportVm({
    required this.viewMode,
    required this.viewModeEnabled,
    required this.textureId,
    required this.viewportState,
    required this.layout,
    required this.tracks,
    required this.quickMarks,
    required this.quickMarkDraft,
    required this.selectedQuickMarkId,
  });
}

class MainWindowMarksVm {
  final List<QuickMark> allMarks;
  final List<QuickMark> visibleMarks;
  final Set<int> visibleMarkIds;
  final int? selectedMarkId;
  final Map<int, TrackInfo> tracksByFileId;
  final Map<int, QuickMarkThumbnail> thumbnailsByMarkId;
  final int currentPtsUs;

  const MainWindowMarksVm({
    required this.allMarks,
    required this.visibleMarks,
    required this.visibleMarkIds,
    required this.selectedMarkId,
    required this.tracksByFileId,
    required this.thumbnailsByMarkId,
    required this.currentPtsUs,
  });
}

class MainWindowMediaVm {
  final bool analysisEnabled;
  final bool analysisOverlayEnabled;
  final bool nativePlaybackAvailable;
  final bool localFilePlaybackAvailable;
  final bool networkMediaAvailable;
  final bool sshRemoteMediaAvailable;
  final bool nativeFilePickerAvailable;
  final PlatformCapability localFilePlaybackCapability;
  final PlatformCapability networkMediaPlaybackCapability;
  final PlatformCapability sshRemoteMediaPlaybackCapability;
  final PlatformCapability nativeFilePickerCapability;
  final PlatformCapability externalAnalysisWindowsCapability;
  final PlatformCapability analysisOverlaysCapability;
  final List<TrackEntry> tracks;
  final Map<int, int> syncOffsets; // fileId -> offset in microseconds
  final int? audibleTrackFileId;
  final PerformanceAlertPolicy performanceAlertPolicy;
  final AnalysisToolbarDataSource analysisDataSource;

  const MainWindowMediaVm({
    required this.analysisEnabled,
    required this.analysisOverlayEnabled,
    required this.nativePlaybackAvailable,
    required this.localFilePlaybackAvailable,
    required this.networkMediaAvailable,
    required this.sshRemoteMediaAvailable,
    required this.nativeFilePickerAvailable,
    required this.localFilePlaybackCapability,
    required this.networkMediaPlaybackCapability,
    required this.sshRemoteMediaPlaybackCapability,
    required this.nativeFilePickerCapability,
    required this.externalAnalysisWindowsCapability,
    required this.analysisOverlaysCapability,
    required this.tracks,
    required this.syncOffsets,
    required this.audibleTrackFileId,
    required this.performanceAlertPolicy,
    required this.analysisDataSource,
  });
}

class MainWindowPlaybackVm {
  final double timelineStartWidth;
  final bool isPlaying;
  final int currentPtsUs;
  final int durationUs;
  final List<int> markerUs;
  final int? seekMinUs;
  final int? seekMaxUs;
  final bool loopRangeEnabled;
  final int loopStartUs;
  final int loopEndUs;
  final double controlsWidth;

  const MainWindowPlaybackVm({
    required this.timelineStartWidth,
    required this.isPlaying,
    required this.currentPtsUs,
    required this.durationUs,
    required this.markerUs,
    required this.seekMinUs,
    required this.seekMaxUs,
    required this.loopRangeEnabled,
    required this.loopStartUs,
    required this.loopEndUs,
    required this.controlsWidth,
  });
}

class MainWindowOverlayVm {
  final bool dragging;
  final bool mediaInfoVisible;
  final bool profilerVisible;
  final bool settingsVisible;
  final bool analysisOverlayControlsVisible;
  final bool marksSidebarVisible;
  final double marksSidebarWidth;
  final bool fullScreen;
  final bool fullScreenControlsVisible;

  const MainWindowOverlayVm({
    required this.dragging,
    required this.mediaInfoVisible,
    required this.profilerVisible,
    required this.settingsVisible,
    required this.analysisOverlayControlsVisible,
    required this.marksSidebarVisible,
    required this.marksSidebarWidth,
    required this.fullScreen,
    required this.fullScreenControlsVisible,
  });
}

class MainWindowViewActions {
  final MainWindowDropActions drop;
  final MainWindowToolbarActions toolbar;
  final MainWindowViewportActions viewport;
  final MainWindowMarksActions marks;
  final MainWindowMediaTimelineActions mediaTimeline;
  final MainWindowAnalysisOverlayActions analysisOverlay;
  final MainWindowOverlayActions overlays;

  const MainWindowViewActions({
    required this.drop,
    required this.toolbar,
    required this.viewport,
    required this.marks,
    required this.mediaTimeline,
    required this.analysisOverlay,
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
  final Future<void> Function() onAnalysisOverlayPanelToggle;
  final VoidCallback onProfiler;
  final VoidCallback onSettings;
  final VoidCallback onMarksSidebarToggle;

  const MainWindowToolbarActions({
    required this.onViewModeChanged,
    required this.onOpenFile,
    required this.onOpenNetworkMedia,
    required this.onOpenSshRemoteMedia,
    required this.onMediaInfo,
    required this.onAnalysis,
    required this.onAnalysisOverlayPanelToggle,
    required this.onProfiler,
    required this.onSettings,
    required this.onMarksSidebarToggle,
  });
}

class MainWindowViewportActions {
  final ValueChanged<Offset> onPan;
  final ValueChanged<double> onSplit;
  final void Function(double scrollDelta, Offset localPos) onZoom;
  final void Function(bool panning, bool splitting) onPointerButton;
  final void Function(int width, int height, double devicePixelRatio) onResize;
  final ValueChanged<Offset> onQuickMarkStart;
  final ValueChanged<Offset> onQuickMarkUpdate;
  final VoidCallback onQuickMarkEnd;
  final VoidCallback onQuickMarkCancel;
  final ValueChanged<int?> onQuickMarkSelect;
  final ValueChanged<QuickMark> onQuickMarkChanged;
  final ValueChanged<int> onQuickMarkDeleted;
  final ValueChanged<int> onQuickMarkFocus;

  const MainWindowViewportActions({
    required this.onPan,
    required this.onSplit,
    required this.onZoom,
    required this.onPointerButton,
    required this.onResize,
    required this.onQuickMarkStart,
    required this.onQuickMarkUpdate,
    required this.onQuickMarkEnd,
    required this.onQuickMarkCancel,
    required this.onQuickMarkSelect,
    required this.onQuickMarkChanged,
    required this.onQuickMarkDeleted,
    required this.onQuickMarkFocus,
  });
}

class MainWindowMarksActions {
  final ValueChanged<int> onJumpToMark;
  final ValueChanged<int?> onSelectVisibleMark;
  final ValueChanged<QuickMark> onMarkChanged;
  final ValueChanged<int> onMarkDeleted;
  final ValueChanged<int> onFocusVisibleMark;

  const MainWindowMarksActions({
    required this.onJumpToMark,
    required this.onSelectVisibleMark,
    required this.onMarkChanged,
    required this.onMarkDeleted,
    required this.onFocusVisibleMark,
  });
}

class MainWindowMediaTimelineActions {
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final Future<void> Function(int fileId) onRemoveTrack;
  final ValueChanged<double> onZoomChanged;
  final VoidCallback onToggleFullScreen;
  final Future<void> Function() onTogglePlay;
  final Future<void> Function() onStepForward;
  final Future<void> Function() onStepBackward;
  final ValueChanged<int> onSeek;
  final void Function(int hoverUs, bool hovering) onSliderHover;
  final Future<void> Function(bool enabled) onLoopRangeEnabledChanged;
  final void Function(int startUs, int endUs) onLoopRangeChanged;
  final Future<void> Function(LoopRangeHandle handle)? onLoopRangeChangeEnd;
  final void Function(int oldIndex, int newIndex) onReorder;
  final Future<void> Function(int slot, int offsetMs) onOffsetChanged;
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

class MainWindowAnalysisOverlayActions {
  final ValueChanged<AnalysisOverlayType> onTypeChanged;
  final ValueChanged<double> onOpacityChanged;
  final Future<void> Function() onActivate;
  final VoidCallback onClose;

  const MainWindowAnalysisOverlayActions({
    required this.onTypeChanged,
    required this.onOpacityChanged,
    required this.onActivate,
    required this.onClose,
  });
}

class MainWindowOverlayActions {
  final VoidCallback onCloseMediaInfo;
  final VoidCallback onCloseProfiler;
  final VoidCallback onCloseSettings;
  final VoidCallback onCloseMarksSidebar;
  final ValueChanged<double> onMarksSidebarWidthChanged;
  final ValueChanged<ViewportPixelSizeMode> onViewportPixelSizeModeChanged;
  final ValueChanged<PerformanceAlertPolicy> onPerformanceAlertPolicyChanged;
  final VoidCallback onFullScreenPointerActivity;
  final void Function(bool hovering) onFullScreenControlsHoverChanged;

  const MainWindowOverlayActions({
    required this.onCloseMediaInfo,
    required this.onCloseProfiler,
    required this.onCloseSettings,
    required this.onCloseMarksSidebar,
    required this.onMarksSidebarWidthChanged,
    required this.onViewportPixelSizeModeChanged,
    required this.onPerformanceAlertPolicyChanged,
    required this.onFullScreenPointerActivity,
    required this.onFullScreenControlsHoverChanged,
  });
}
