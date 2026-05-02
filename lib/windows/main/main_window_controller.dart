import 'dart:async';

import 'package:flutter/material.dart';
import 'package:window_manager/window_manager.dart';

import '../../actions/test_runner.dart';
import '../../startup_options.dart';
import '../../track_manager.dart';
import '../../utils/async_guard.dart';
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

class MainWindowController {
  final TickerProvider vsync;
  final StartupOptions startupOptions;
  final bool Function() mounted;

  final NativePlayerController player = NativePlayerController();
  final TrackManager trackManager = TrackManager();
  final MainWindowStateStore stateStore = MainWindowStateStore();
  final ValueNotifier<TimelineHoverState> timelineHoverNotifier = ValueNotifier(
    const TimelineHoverState(),
  );
  final GlobalKey timelineSliderKey = GlobalKey();
  final GlobalKey loopRangeBarKey = GlobalKey();
  final GlobalKey viewportKey = GlobalKey();
  Timer? _fullScreenControlsTimer;
  int _fullScreenSerial = 0;
  bool? _pendingFullScreen;
  bool _fullScreenUiResizePending = false;
  int? _windowedViewportWidth;
  int? _windowedViewportHeight;

  late final MainWindowAnalysisCoordinator analysisCoordinator;
  late final MainWindowTestHarness testHarness;
  late final MainWindowLayoutCoordinator layoutCoordinator;
  late final MainWindowMediaCoordinator mediaCoordinator;
  late final MainWindowPlaybackCoordinator playbackCoordinator;
  late final MainWindowActionCoordinator actionCoordinator;
  late final MainWindowViewActions _viewActions = _createViewActions();

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
    _fullScreenControlsTimer?.cancel();
    actionCoordinator.dispose();
    playbackCoordinator.dispose();
    mediaCoordinator.dispose();
    layoutCoordinator.dispose();
    timelineHoverNotifier.dispose();
    stateStore.dispose();
    fireAndLog('dispose analysis coordinator', analysisCoordinator.dispose());
    trackManager.dispose();
    fireAndLog('dispose player', player.dispose());
  }

  void setViewportBackgroundColor(Color color) {
    fireAndLog(
      'set viewport background color',
      player.setViewportBackgroundColor(color.toARGB32()),
    );
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
      tracks: trackManager.entries,
      viewportKey: viewportKey,
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
      timelineHoverListenable: timelineHoverNotifier,
      controlsWidth: _timelineControlsWidth,
      profilerVisible: _profilerVisible,
      settingsVisible: _settingsVisible,
      fullScreen: _fullScreen,
      fullScreenControlsVisible: _fullScreenControlsVisible,
      audibleTrackFileId: _audibleTrackFileId,
    );
  }

  MainWindowViewActions get viewActions => _viewActions;

  MainWindowViewActions _createViewActions() {
    return MainWindowViewActions(
      onFilesDropped: (paths) {
        stateStore.setDragging(false);
        fireAndLog(
          'load dropped media',
          mediaCoordinator.loadMediaPaths(paths),
        );
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
      onProfiler: () => stateStore.setProfilerVisible(!_profilerVisible),
      onCloseProfiler: () => stateStore.setProfilerVisible(false),
      onSettings: () => stateStore.setSettingsVisible(!_settingsVisible),
      onCloseSettings: () => stateStore.setSettingsVisible(false),
      onPan: layoutCoordinator.onPan,
      onSplit: layoutCoordinator.onSplit,
      onZoom: layoutCoordinator.onZoom,
      onPointerButton: layoutCoordinator.onPointerButton,
      onResize: (width, height, devicePixelRatio) =>
          layoutCoordinator.onViewportResize(
            width,
            height,
            devicePixelRatio,
            immediate: _fullScreenUiResizePending,
          ),
      onMediaSwapped: mediaCoordinator.onMediaSwapped,
      onRemoveTrack: mediaCoordinator.removeTrack,
      onZoomChanged: layoutCoordinator.onZoomComboChanged,
      onToggleFullScreen: _toggleFullScreen,
      onFullScreenPointerActivity: _showFullScreenControlsTemporarily,
      onFullScreenControlsHoverChanged: _setFullScreenControlsHovering,
      onTogglePlay: playbackCoordinator.togglePlayPause,
      onStepForward: () => player.stepForward(),
      onStepBackward: () => player.stepBackward(),
      onSeek: playbackCoordinator.seekTo,
      onSliderHover: playbackCoordinator.onSliderHover,
      onLoopRangeEnabledChanged: (enabled) => fireAndLog(
        'set loop range enabled',
        playbackCoordinator.setLoopRangeEnabled(enabled),
      ),
      onLoopRangeChanged: playbackCoordinator.previewLoopRange,
      onLoopRangeChangeEnd: (handle) {
        if (!_loopRangeEnabled) return;
        fireAndLog(
          'finish loop range change',
          playbackCoordinator.commitLoopRange(
            seekToStart: handle == LoopRangeHandle.start,
          ),
        );
      },
      onReorder: trackManager.moveTrack,
      onOffsetChanged: mediaCoordinator.onOffsetChanged,
      onToggleTrackAudio: _toggleTrackAudio,
      onControlsWidthChanged: stateStore.setTimelineControlsWidth,
    );
  }

  void _toggleTrackAudio(int fileId) {
    final next = _audibleTrackFileId == fileId ? null : fileId;
    stateStore.setAudibleTrackFileId(next);
    fireAndLog('set audible track', player.setAudibleTrack(next));
  }

  void _toggleFullScreen() {
    final currentTarget = _pendingFullScreen ?? _fullScreen;
    _requestFullScreen(!currentTarget, reason: 'toggle full screen');
  }

  void _exitFullScreen() {
    final currentTarget = _pendingFullScreen ?? _fullScreen;
    if (!currentTarget) return;
    _requestFullScreen(false, reason: 'exit full screen');
  }

  void _requestFullScreen(bool fullScreen, {required String reason}) {
    _fullScreenSerial++;
    _pendingFullScreen = fullScreen;
    fireAndLog(reason, _setFullScreen(fullScreen, _fullScreenSerial));
  }

  Future<void> _setFullScreen(bool fullScreen, int serial) async {
    _fullScreenControlsTimer?.cancel();
    try {
      if (fullScreen) {
        _rememberWindowedViewportSize();
      }
      // Switch the native window first so the Flutter fullscreen chrome never
      // renders inside the old, non-fullscreen bounds.
      await windowManager.setFullScreen(fullScreen);
      if (!mounted() || serial != _fullScreenSerial) return;
      if (fullScreen) {
        await _preemptFullScreenViewportResize();
      } else {
        await _preemptWindowedViewportResize();
      }
      if (!mounted() || serial != _fullScreenSerial) return;
      _fullScreenUiResizePending = true;
      stateStore.setFullScreen(fullScreen);
      await WidgetsBinding.instance.endOfFrame;
      if (!mounted() || serial != _fullScreenSerial) return;
      await _preemptMeasuredViewportResize();
      if (!mounted() || serial != _fullScreenSerial) return;
      if (fullScreen) {
        _scheduleFullScreenControlsHide();
      }
    } finally {
      if (serial == _fullScreenSerial) {
        _fullScreenUiResizePending = false;
        _pendingFullScreen = null;
      }
    }
  }

  void _rememberWindowedViewportSize() {
    if (layoutCoordinator.viewportWidth <= 0 ||
        layoutCoordinator.viewportHeight <= 0) {
      return;
    }
    _windowedViewportWidth = layoutCoordinator.viewportWidth;
    _windowedViewportHeight = layoutCoordinator.viewportHeight;
  }

  Future<void> _preemptFullScreenViewportResize() async {
    final dpr = layoutCoordinator.viewportDevicePixelRatio;
    if (dpr <= 0) return;
    final bounds = await windowManager.getBounds();
    await layoutCoordinator.preemptViewportResize(
      width: (bounds.width * dpr).round(),
      height: (bounds.height * dpr).round(),
    );
  }

  Future<void> _preemptWindowedViewportResize() async {
    final width = _windowedViewportWidth;
    final height = _windowedViewportHeight;
    if (width == null || height == null) return;
    await layoutCoordinator.preemptViewportResize(width: width, height: height);
  }

  Future<void> _preemptMeasuredViewportResize() async {
    final context = viewportKey.currentContext;
    if (context == null) return;
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) return;
    final size = renderObject.size;
    if (size.width <= 0 || size.height <= 0) return;
    final dpr = View.of(context).devicePixelRatio;
    if (dpr <= 0) return;
    await layoutCoordinator.preemptViewportResize(
      width: (size.width * dpr).round(),
      height: (size.height * dpr).round(),
    );
  }

  void _showFullScreenControlsTemporarily() {
    if (!_fullScreen) return;
    stateStore.setFullScreenControlsVisible(true);
    _scheduleFullScreenControlsHide();
  }

  void _setFullScreenControlsHovering(bool hovering) {
    if (!_fullScreen) return;
    if (hovering) {
      _fullScreenControlsTimer?.cancel();
      stateStore.setFullScreenControlsVisible(true);
    } else {
      _scheduleFullScreenControlsHide();
    }
  }

  void _scheduleFullScreenControlsHide() {
    _fullScreenControlsTimer?.cancel();
    _fullScreenControlsTimer = Timer(const Duration(seconds: 1), () {
      if (!_fullScreen || !mounted()) return;
      stateStore.setFullScreenControlsVisible(false);
    });
  }

  void _initCoordinators() {
    layoutCoordinator = MainWindowLayoutCoordinator(
      vsync: vsync,
      controller: player,
      mounted: mounted,
      textureId: () => _textureId,
      layout: () => _layout,
      setLayout: stateStore.setLayout,
      trackCount: () => trackManager.count,
      tracks: () => trackManager.entries,
    );
    analysisCoordinator = MainWindowAnalysisCoordinator(
      trackManager: trackManager,
    );
    playbackCoordinator = MainWindowPlaybackCoordinator(
      controller: player,
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
      hoverPtsUs: () => timelineHoverNotifier.value.hoverPtsUs,
      sliderHovering: () => timelineHoverNotifier.value.sliderHovering,
      setSliderHoverState: _setTimelineHover,
    );
    mediaCoordinator = MainWindowMediaCoordinator(
      controller: player,
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
      audibleTrackFileId: () => _audibleTrackFileId,
      setAudibleTrackFileId: stateStore.setAudibleTrackFileId,
      applyStartupLoopRangeIfReady:
          playbackCoordinator.applyStartupLoopRangeIfReady,
      cancelLoopBoundaryTimer: playbackCoordinator.cancelLoopBoundaryTimer,
      resetAfterLastTrackRemoved: _resetAfterLastTrackRemoved,
      seekTo: playbackCoordinator.seekTo,
    );
    testHarness = MainWindowTestHarness(
      viewportKey: viewportKey,
      timelineSliderKey: timelineSliderKey,
      loopRangeBarKey: loopRangeBarKey,
      splitPosition: () => _layout.splitPos,
      timelineStartWidth: () => _timelineStartWidth,
      effectiveDurationUs: () => mediaCoordinator.effectiveDurationUs,
      resolvedLoopStartUs: () => _resolvedLoopStartUs,
      resolvedLoopEndUs: () => _resolvedLoopEndUs,
    );
    actionCoordinator = MainWindowActionCoordinator(
      controller: player,
      playbackCoordinator: playbackCoordinator,
      mediaCoordinator: mediaCoordinator,
      layoutCoordinator: layoutCoordinator,
      analysisCoordinator: analysisCoordinator,
      testHarness: testHarness,
      isLoopRangeEnabled: () => _loopRangeEnabled,
      showProfilerOverlay: () => stateStore.setProfilerVisible(true),
      showSettingsDialog: () => stateStore.setSettingsVisible(true),
      toggleFullScreen: _toggleFullScreen,
      exitFullScreen: _exitFullScreen,
      openNewWindow: () {},
    );
  }

  void _maybeStartTestRunner(String? path) {
    if (path == null) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      TestRunner(scriptPath: path, controller: player).run();
    });
  }

  void _onTrackManagerChanged() {
    stateStore.setLayout(_layout.copyWith(order: trackManager.order));
    layoutCoordinator.markLayoutDirty();
    fireAndLog(
      'publish analysis track snapshot',
      analysisCoordinator.publishTrackSnapshot(),
    );
  }

  void _resetAfterLastTrackRemoved() {
    if (_fullScreen) {
      _requestFullScreen(
        false,
        reason: 'exit full screen after last track removed',
      );
    }
    trackManager.clear();
    stateStore.resetAfterLastTrackRemoved();
    playbackCoordinator.invalidateLoopRangeSync();
  }

  MainWindowStateModel get _state => stateStore.value;

  void _setTimelineHover(int hoverUs, bool hovering) {
    final next = TimelineHoverState(
      hoverPtsUs: hoverUs,
      sliderHovering: hovering,
    );
    if (timelineHoverNotifier.value == next) return;
    timelineHoverNotifier.value = next;
  }

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
  bool get _dragging => _state.dragging;
  bool get _profilerVisible => _state.profilerVisible;
  bool get _settingsVisible => _state.settingsVisible;
  bool get _fullScreen => _state.fullScreen;
  bool get _fullScreenControlsVisible => _state.fullScreenControlsVisible;
  int? get _audibleTrackFileId => _state.audibleTrackFileId;
  double get _timelineStartWidth => playbackCoordinator.timelineStartWidth;
  int get _resolvedLoopStartUs => playbackCoordinator.resolvedLoopStartUs;
  int get _resolvedLoopEndUs => playbackCoordinator.resolvedLoopEndUs;
  List<int> get _loopMarkerPtsUs => playbackCoordinator.loopMarkerPtsUs;
}
