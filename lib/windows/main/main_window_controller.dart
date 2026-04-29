import 'dart:async';

import 'package:flutter/material.dart';

import '../../actions/test_runner.dart';
import '../../startup_options.dart';
import '../../track_manager.dart';
import '../../video_renderer_controller.dart';
import '../../widgets/loop_range_bar.dart';
import 'main_window_actions.dart';
import 'main_window_analysis.dart';
import 'main_window_layout.dart';
import 'main_window_media.dart';
import 'main_window_playback.dart';
import 'main_window_state.dart';
import 'main_window_test_hooks.dart';
import 'main_window_view.dart';
import '../window_manager.dart';

class MainWindowController {
  final TickerProvider vsync;
  final StartupOptions startupOptions;
  final bool Function() mounted;

  final VideoRendererController renderer = VideoRendererController();
  final TrackManager trackManager = TrackManager();
  final MainWindowStateStore stateStore = MainWindowStateStore();
  final GlobalKey timelineSliderKey = GlobalKey();
  final GlobalKey loopRangeBarKey = GlobalKey();

  late final MainWindowAnalysisCoordinator analysisCoordinator;
  late final MainWindowTestHarness testHarness;
  late final MainWindowLayoutCoordinator layoutCoordinator;
  late final MainWindowMediaCoordinator mediaCoordinator;
  late final MainWindowPlaybackCoordinator playbackCoordinator;
  late final MainWindowActionCoordinator actionCoordinator;

  MainWindowController({
    required this.vsync,
    required this.startupOptions,
    required this.mounted,
  }) {
    _initCoordinators();
  }

  Listenable get listenable => stateStore;

  void start({String? testScriptPath}) {
    trackManager.addListener(_onTrackManagerChanged);
    actionCoordinator.bind();
    playbackCoordinator.startPolling();
    _maybeStartTestRunner(testScriptPath);
  }

  void dispose() {
    actionCoordinator.dispose();
    playbackCoordinator.dispose();
    mediaCoordinator.dispose();
    layoutCoordinator.dispose();
    stateStore.dispose();
    unawaited(analysisCoordinator.dispose());
    trackManager.dispose();
    unawaited(renderer.dispose());
  }

  MainWindowViewModel get viewModel {
    return MainWindowViewModel(
      dragging: _dragging,
      viewMode: _layout.mode,
      viewModeEnabled: _textureId != null,
      analysisEnabled: trackManager.count > 0,
      textureId: _textureId,
      viewportState: _viewportState,
      layout: _layout,
      trackManager: trackManager,
      timelineSliderKey: timelineSliderKey,
      timelineStartWidth: _timelineStartWidth,
      isPlaying: _isPlaying,
      currentPtsUs: _currentPtsUs,
      durationUs: mediaCoordinator.effectiveDurationUs,
      markerUs: _loopMarkerPtsUs,
      seekMinUs: _loopRangeEnabled ? _resolvedLoopStartUs : null,
      seekMaxUs: _loopRangeEnabled ? _resolvedLoopEndUs : null,
      loopRangeBarKey: loopRangeBarKey,
      loopRangeEnabled: _loopRangeEnabled,
      loopStartUs: _resolvedLoopStartUs,
      loopEndUs: _resolvedLoopEndUs,
      syncOffsets: _syncOffsets,
      hoverPtsUs: _hoverPtsUs,
      sliderHovering: _sliderHovering,
      controlsWidth: _timelineControlsWidth,
    );
  }

  MainWindowViewActions get viewActions {
    return MainWindowViewActions(
      onFilesDropped: (paths) {
        stateStore.setDragging(false);
        unawaited(mediaCoordinator.loadMediaPaths(paths));
      },
      onDragEntered: () {
        if (!_dragging) stateStore.setDragging(true);
      },
      onDragExited: () {
        if (_dragging) stateStore.setDragging(false);
      },
      onViewModeChanged: (mode) {
        stateStore.setLayout(_layout.copyWith(mode: mode));
        layoutCoordinator.markLayoutDirty();
      },
      onAddMedia: mediaCoordinator.openFile,
      onAnalysis: analysisCoordinator.triggerAnalysis,
      onProfiler: () => WindowManager.showStatsWindow(),
      onSettings: () => WindowManager.showSettingsWindow(),
      onPan: layoutCoordinator.onPan,
      onSplit: layoutCoordinator.onSplit,
      onZoom: layoutCoordinator.onZoom,
      onPointerButton: layoutCoordinator.onPointerButton,
      onResize: layoutCoordinator.onViewportResize,
      onMediaSwapped: mediaCoordinator.onMediaSwapped,
      onRemoveTrack: mediaCoordinator.removeTrack,
      onZoomChanged: layoutCoordinator.onZoomComboChanged,
      onTogglePlay: playbackCoordinator.togglePlayPause,
      onStepForward: () => renderer.stepForward(),
      onStepBackward: () => renderer.stepBackward(),
      onSeek: playbackCoordinator.seekTo,
      onSliderHover: playbackCoordinator.onSliderHover,
      onLoopRangeEnabledChanged: (enabled) =>
          unawaited(playbackCoordinator.setLoopRangeEnabled(enabled)),
      onLoopRangeChanged: (startUs, endUs) =>
          unawaited(playbackCoordinator.setLoopRange(startUs, endUs)),
      onLoopRangeChangeEnd: _loopRangeEnabled
          ? (handle) => unawaited(
              playbackCoordinator.setLoopRange(
                _resolvedLoopStartUs,
                _resolvedLoopEndUs,
                seekToStart: handle == LoopRangeHandle.start,
              ),
            )
          : null,
      onReorder: trackManager.moveTrack,
      onOffsetChanged: mediaCoordinator.onOffsetChanged,
      onControlsWidthChanged: stateStore.setTimelineControlsWidth,
    );
  }

  void _initCoordinators() {
    layoutCoordinator = MainWindowLayoutCoordinator(
      vsync: vsync,
      controller: renderer,
      mounted: mounted,
      textureId: () => _textureId,
      layout: () => _layout,
      setLayout: stateStore.setLayout,
      trackCount: () => trackManager.count,
    );
    analysisCoordinator = MainWindowAnalysisCoordinator(
      trackManager: trackManager,
    );
    playbackCoordinator = MainWindowPlaybackCoordinator(
      controller: renderer,
      trackManager: trackManager,
      startupOptions: startupOptions,
      mounted: mounted,
      textureId: () => _textureId,
      effectiveDurationUs: () => mediaCoordinator.effectiveDurationUs,
      timelineControlsWidth: () => _timelineControlsWidth,
      isPlaying: () => _isPlaying,
      setPlaying: stateStore.setPlaying,
      playbackSpeed: () => _playbackSpeed,
      setPlaybackSpeed: stateStore.setPlaybackSpeed,
      currentPtsUs: () => _currentPtsUs,
      durationUs: () => _durationUs,
      pendingSeekUs: () => _pendingSeekUs,
      pendingSeekAt: () => _pendingSeekAt,
      setSeekPreview: stateStore.setSeekPreview,
      setPendingSeek: stateStore.setPendingSeek,
      setPolledPlaybackState: stateStore.setPolledPlaybackState,
      loopRangeEnabled: () => _loopRangeEnabled,
      setLoopRangeEnabledState: stateStore.setLoopRangeEnabled,
      nativeLoopRangeSynced: () => _nativeLoopRangeSynced,
      setNativeLoopRangeSynced: stateStore.setNativeLoopRangeSynced,
      startupLoopRangeApplied: () => _startupLoopRangeApplied,
      setStartupLoopRangeApplied: stateStore.setStartupLoopRangeApplied,
      loopStartUs: () => _loopStartUs,
      loopEndUs: () => _loopEndUs,
      setLoopRangeState: stateStore.setLoopRange,
      hoverPtsUs: () => _hoverPtsUs,
      sliderHovering: () => _sliderHovering,
      setSliderHoverState: stateStore.setSliderHover,
    );
    mediaCoordinator = MainWindowMediaCoordinator(
      controller: renderer,
      trackManager: trackManager,
      layoutCoordinator: layoutCoordinator,
      mounted: mounted,
      textureId: () => _textureId,
      setViewportState: stateStore.setViewportState,
      setTextureId: stateStore.setTextureId,
      setLayout: stateStore.setLayout,
      syncOffsets: () => _syncOffsets,
      setSyncOffsets: stateStore.setSyncOffsets,
      durationUs: () => _durationUs,
      pendingSeekUs: () => _pendingSeekUs,
      currentPtsUs: () => _currentPtsUs,
      applyStartupLoopRangeIfReady:
          playbackCoordinator.applyStartupLoopRangeIfReady,
      cancelLoopBoundaryTimer: playbackCoordinator.cancelLoopBoundaryTimer,
      resetAfterLastTrackRemoved: _resetAfterLastTrackRemoved,
      seekTo: playbackCoordinator.seekTo,
    );
    testHarness = MainWindowTestHarness(
      timelineSliderKey: timelineSliderKey,
      loopRangeBarKey: loopRangeBarKey,
      timelineStartWidth: () => _timelineStartWidth,
      effectiveDurationUs: () => mediaCoordinator.effectiveDurationUs,
      resolvedLoopStartUs: () => _resolvedLoopStartUs,
      resolvedLoopEndUs: () => _resolvedLoopEndUs,
    );
    actionCoordinator = MainWindowActionCoordinator(
      controller: renderer,
      playbackCoordinator: playbackCoordinator,
      mediaCoordinator: mediaCoordinator,
      layoutCoordinator: layoutCoordinator,
      analysisCoordinator: analysisCoordinator,
      testHarness: testHarness,
      isLoopRangeEnabled: () => _loopRangeEnabled,
    );
  }

  void _maybeStartTestRunner(String? path) {
    if (path == null) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      TestRunner(scriptPath: path, controller: renderer).run();
    });
  }

  void _onTrackManagerChanged() {
    stateStore.setLayout(_layout.copyWith(order: trackManager.order));
    layoutCoordinator.markLayoutDirty();
    unawaited(analysisCoordinator.publishTrackSnapshot());
  }

  void _resetAfterLastTrackRemoved() {
    trackManager.clear();
    stateStore.resetAfterLastTrackRemoved();
    playbackCoordinator.invalidateLoopRangeSync();
  }

  MainWindowStateModel get _state => stateStore.value;

  int? get _textureId => _state.textureId;
  int get _viewportState => _state.viewportState;
  bool get _isPlaying => _state.isPlaying;
  double get _playbackSpeed => _state.playbackSpeed;
  int get _currentPtsUs => _state.currentPtsUs;
  int get _durationUs => _state.durationUs;
  LayoutState get _layout => _state.layout;
  int? get _pendingSeekUs => _state.pendingSeekUs;
  DateTime? get _pendingSeekAt => _state.pendingSeekAt;
  Map<int, int> get _syncOffsets => _state.syncOffsets;
  double get _timelineControlsWidth => _state.timelineControlsWidth;
  bool get _loopRangeEnabled => _state.loopRangeEnabled;
  bool get _nativeLoopRangeSynced => _state.nativeLoopRangeSynced;
  bool get _startupLoopRangeApplied => _state.startupLoopRangeApplied;
  int get _loopStartUs => _state.loopStartUs;
  int get _loopEndUs => _state.loopEndUs;
  int get _hoverPtsUs => _state.hoverPtsUs;
  bool get _sliderHovering => _state.sliderHovering;
  bool get _dragging => _state.dragging;
  double get _timelineStartWidth => playbackCoordinator.timelineStartWidth;
  int get _resolvedLoopStartUs => playbackCoordinator.resolvedLoopStartUs;
  int get _resolvedLoopEndUs => playbackCoordinator.resolvedLoopEndUs;
  List<int> get _loopMarkerPtsUs => playbackCoordinator.loopMarkerPtsUs;
}
