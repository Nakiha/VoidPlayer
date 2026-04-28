import 'dart:async';
import 'package:flutter/material.dart';
import '../startup_options.dart';
import '../video_renderer_controller.dart';
import '../track_manager.dart';
import '../actions/test_runner.dart';
import 'window_manager.dart';
import '../widgets/loop_range_bar.dart';
import 'main_window_analysis.dart';
import 'main_window_actions.dart';
import 'main_window_layout.dart';
import 'main_window_media.dart';
import 'main_window_playback.dart';
import 'main_window_state.dart' as main_state;
import 'main_window_test_hooks.dart' as main_hooks;
import 'main_window_view.dart';

class MainWindow extends StatefulWidget {
  final String? testScriptPath;
  final StartupOptions startupOptions;

  const MainWindow({
    super.key,
    this.testScriptPath,
    this.startupOptions = const StartupOptions(),
  });

  @override
  State<MainWindow> createState() => _MainWindowState();
}

class _MainWindowState extends State<MainWindow> with TickerProviderStateMixin {
  final _controller = VideoRendererController();
  final _trackManager = TrackManager();
  final _timelineSliderKey = GlobalKey();
  final _loopRangeBarKey = GlobalKey();
  late final MainWindowAnalysisCoordinator _analysisCoordinator;
  late final main_hooks.MainWindowTestHarness _testHarness;
  late final MainWindowLayoutCoordinator _layoutCoordinator;
  late final MainWindowMediaCoordinator _mediaCoordinator;
  late final MainWindowPlaybackCoordinator _playbackCoordinator;

  main_state.MainWindowStateModel _state =
      const main_state.MainWindowStateModel();

  @override
  void initState() {
    super.initState();
    _layoutCoordinator = MainWindowLayoutCoordinator(
      vsync: this,
      controller: _controller,
      mounted: () => mounted,
      textureId: () => _textureId,
      layout: () => _layout,
      setLayout: (layout) => setState(() => _layout = layout),
      trackCount: () => _trackManager.count,
    );
    _analysisCoordinator = MainWindowAnalysisCoordinator(
      trackManager: _trackManager,
    );
    _playbackCoordinator = MainWindowPlaybackCoordinator(
      controller: _controller,
      trackManager: _trackManager,
      startupOptions: widget.startupOptions,
      mounted: () => mounted,
      textureId: () => _textureId,
      effectiveDurationUs: () => _mediaCoordinator.effectiveDurationUs,
      timelineControlsWidth: () => _timelineControlsWidth,
      isPlaying: () => _isPlaying,
      setPlaying: _setPlaying,
      playbackSpeed: () => _playbackSpeed,
      setPlaybackSpeed: _setPlaybackSpeed,
      currentPtsUs: () => _currentPtsUs,
      durationUs: () => _durationUs,
      pendingSeekUs: () => _pendingSeekUs,
      pendingSeekAt: () => _pendingSeekAt,
      setSeekPreview: _setSeekPreview,
      setPendingSeek: _setPendingSeekState,
      setPolledPlaybackState: _setPolledPlaybackState,
      loopRangeEnabled: () => _loopRangeEnabled,
      setLoopRangeEnabledState: _setLoopRangeEnabledState,
      nativeLoopRangeSynced: () => _nativeLoopRangeSynced,
      setNativeLoopRangeSynced: _setNativeLoopRangeSynced,
      startupLoopRangeApplied: () => _startupLoopRangeApplied,
      setStartupLoopRangeApplied: _setStartupLoopRangeApplied,
      loopStartUs: () => _loopStartUs,
      loopEndUs: () => _loopEndUs,
      setLoopRangeState: _setLoopRangeState,
      hoverPtsUs: () => _hoverPtsUs,
      sliderHovering: () => _sliderHovering,
      setSliderHoverState: _setSliderHoverState,
    );
    _mediaCoordinator = MainWindowMediaCoordinator(
      controller: _controller,
      trackManager: _trackManager,
      layoutCoordinator: _layoutCoordinator,
      mounted: () => mounted,
      textureId: () => _textureId,
      setViewportState: _setViewportState,
      setTextureId: _setTextureId,
      setLayout: (layout) => setState(() => _layout = layout),
      syncOffsets: () => _syncOffsets,
      setSyncOffsets: (offsets) => setState(() => _syncOffsets = offsets),
      durationUs: () => _durationUs,
      pendingSeekUs: () => _pendingSeekUs,
      currentPtsUs: () => _currentPtsUs,
      applyStartupLoopRangeIfReady:
          _playbackCoordinator.applyStartupLoopRangeIfReady,
      cancelLoopBoundaryTimer: _playbackCoordinator.cancelLoopBoundaryTimer,
      resetAfterLastTrackRemoved: _resetAfterLastTrackRemoved,
      seekTo: _playbackCoordinator.seekTo,
    );
    _testHarness = main_hooks.MainWindowTestHarness(
      timelineSliderKey: _timelineSliderKey,
      loopRangeBarKey: _loopRangeBarKey,
      timelineStartWidth: () => _timelineStartWidth,
      effectiveDurationUs: () => _effectiveDurationUs,
      resolvedLoopStartUs: () => _resolvedLoopStartUs,
      resolvedLoopEndUs: () => _resolvedLoopEndUs,
    );
    _trackManager.addListener(_onTrackManagerChanged);
    _bindActions();
    _playbackCoordinator.startPolling();
    _maybeStartTestRunner();
  }

  void _bindActions() {
    MainWindowActionBinder(
      togglePlayPause: _playbackCoordinator.togglePlayPause,
      play: _playbackCoordinator.play,
      pause: _playbackCoordinator.pause,
      stepForward: _controller.stepForward,
      stepBackward: _controller.stepBackward,
      seekTo: _playbackCoordinator.seekTo,
      clickTimelineFraction: _testHarness.clickTimelineFraction,
      setSpeed: _playbackCoordinator.setSpeed,
      openFile: _mediaCoordinator.openFile,
      addMediaByPath: _mediaCoordinator.addMediaByPath,
      removeTrack: _mediaCoordinator.removeTrack,
      adjustTrackOffset: _mediaCoordinator.onOffsetChanged,
      setLoopRangeEnabled: (enabled) =>
          unawaited(_playbackCoordinator.setLoopRangeEnabled(enabled)),
      isLoopRangeEnabled: () => _loopRangeEnabled,
      setLoopRange:
          (
            startUs,
            endUs, {
            seekToStart = false,
            seekOnlyIfStartChanged = false,
          }) => unawaited(
            _playbackCoordinator.setLoopRange(
              startUs,
              endUs,
              seekToStart: seekToStart,
              seekOnlyIfStartChanged: seekOnlyIfStartChanged,
            ),
          ),
      dragLoopHandle: _testHarness.dragLoopHandle,
      toggleLayoutMode: _layoutCoordinator.toggleLayoutMode,
      setLayoutMode: _layoutCoordinator.setLayoutMode,
      setZoom: _layoutCoordinator.setZoom,
      setSplitPos: _layoutCoordinator.setSplitPos,
      panByDelta: _layoutCoordinator.panByDelta,
      openNewWindow: WindowManager.showStatsWindow,
      openSettings: WindowManager.showSettingsWindow,
      openStats: WindowManager.showStatsWindow,
      openMemory: WindowManager.showMemoryWindow,
      runAnalysis: _analysisCoordinator.triggerAnalysis,
    ).bind();
  }

  void _maybeStartTestRunner() {
    final path = widget.testScriptPath;
    if (path == null) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      TestRunner(scriptPath: path, controller: _controller).run();
    });
  }

  @override
  void dispose() {
    _playbackCoordinator.dispose();
    _layoutCoordinator.dispose();
    unawaited(_analysisCoordinator.dispose());
    _trackManager.dispose();
    unawaited(_controller.dispose());
    super.dispose();
  }

  // -- TrackManager listener --

  void _onTrackManagerChanged() {
    setState(() {
      _layout = _layout.copyWith(order: _trackManager.order);
    });
    _layoutCoordinator.markLayoutDirty();
    unawaited(_analysisCoordinator.publishTrackSnapshot());
  }

  void _setViewportState(int state) {
    setState(() => _viewportState = state);
  }

  void _setTextureId(int textureId) {
    setState(() => _textureId = textureId);
  }

  void _resetAfterLastTrackRemoved() {
    setState(() {
      _trackManager.clear();
      _textureId = null;
      _viewportState = 1;
      _isPlaying = false;
      _currentPtsUs = 0;
      _durationUs = 0;
      _layout = const LayoutState();
      _syncOffsets = {};
      _loopRangeEnabled = false;
      _nativeLoopRangeSynced = false;
      _loopStartUs = 0;
      _loopEndUs = 0;
    });
    _playbackCoordinator.invalidateLoopRangeSync();
  }

  void _setPlaying(bool playing) {
    setState(() => _isPlaying = playing);
  }

  void _setPlaybackSpeed(double speed) {
    setState(() => _playbackSpeed = speed);
  }

  void _setSeekPreview(int ptsUs) {
    setState(() {
      _currentPtsUs = ptsUs;
      _pendingSeekUs = ptsUs;
      _pendingSeekAt = DateTime.now();
    });
  }

  void _setPendingSeekState(int? ptsUs, DateTime? at) {
    setState(() {
      _pendingSeekUs = ptsUs;
      _pendingSeekAt = at;
    });
  }

  void _setTimelineControlsWidth(double width) {
    if (_timelineControlsWidth == width) return;
    setState(() => _timelineControlsWidth = width);
  }

  void _setPolledPlaybackState(int ptsUs, int durationUs, bool playing) {
    setState(() {
      _currentPtsUs = ptsUs;
      _durationUs = durationUs;
      _isPlaying = playing;
    });
  }

  void _setLoopRangeEnabledState(bool enabled) {
    setState(() => _loopRangeEnabled = enabled);
  }

  void _setNativeLoopRangeSynced(bool synced) {
    setState(() => _nativeLoopRangeSynced = synced);
  }

  void _setStartupLoopRangeApplied(bool applied) {
    setState(() => _startupLoopRangeApplied = applied);
  }

  void _setLoopRangeState(int startUs, int endUs) {
    setState(() {
      _loopStartUs = startUs;
      _loopEndUs = endUs;
    });
  }

  void _setSliderHoverState(int hoverUs, bool hovering) {
    setState(() {
      _hoverPtsUs = hoverUs;
      _sliderHovering = hovering;
    });
  }

  int? get _textureId => _state.textureId;
  set _textureId(int? value) => _state = _state.copyWith(textureId: value);

  int get _viewportState => _state.viewportState;
  set _viewportState(int value) =>
      _state = _state.copyWith(viewportState: value);

  bool get _isPlaying => _state.isPlaying;
  set _isPlaying(bool value) => _state = _state.copyWith(isPlaying: value);

  double get _playbackSpeed => _state.playbackSpeed;
  set _playbackSpeed(double value) =>
      _state = _state.copyWith(playbackSpeed: value);

  int get _currentPtsUs => _state.currentPtsUs;
  set _currentPtsUs(int value) => _state = _state.copyWith(currentPtsUs: value);

  int get _durationUs => _state.durationUs;
  set _durationUs(int value) => _state = _state.copyWith(durationUs: value);

  LayoutState get _layout => _state.layout;
  set _layout(LayoutState value) => _state = _state.copyWith(layout: value);

  int? get _pendingSeekUs => _state.pendingSeekUs;
  set _pendingSeekUs(int? value) =>
      _state = _state.copyWith(pendingSeekUs: value);

  DateTime? get _pendingSeekAt => _state.pendingSeekAt;
  set _pendingSeekAt(DateTime? value) =>
      _state = _state.copyWith(pendingSeekAt: value);

  Map<int, int> get _syncOffsets => _state.syncOffsets;
  set _syncOffsets(Map<int, int> value) =>
      _state = _state.copyWith(syncOffsets: value);

  double get _timelineControlsWidth => _state.timelineControlsWidth;
  set _timelineControlsWidth(double value) =>
      _state = _state.copyWith(timelineControlsWidth: value);

  bool get _loopRangeEnabled => _state.loopRangeEnabled;
  set _loopRangeEnabled(bool value) =>
      _state = _state.copyWith(loopRangeEnabled: value);

  bool get _nativeLoopRangeSynced => _state.nativeLoopRangeSynced;
  set _nativeLoopRangeSynced(bool value) =>
      _state = _state.copyWith(nativeLoopRangeSynced: value);

  bool get _startupLoopRangeApplied => _state.startupLoopRangeApplied;
  set _startupLoopRangeApplied(bool value) =>
      _state = _state.copyWith(startupLoopRangeApplied: value);

  int get _loopStartUs => _state.loopStartUs;
  set _loopStartUs(int value) => _state = _state.copyWith(loopStartUs: value);

  int get _loopEndUs => _state.loopEndUs;
  set _loopEndUs(int value) => _state = _state.copyWith(loopEndUs: value);

  int get _hoverPtsUs => _state.hoverPtsUs;
  set _hoverPtsUs(int value) => _state = _state.copyWith(hoverPtsUs: value);

  bool get _sliderHovering => _state.sliderHovering;
  set _sliderHovering(bool value) =>
      _state = _state.copyWith(sliderHovering: value);

  bool get _dragging => _state.dragging;
  set _dragging(bool value) => _state = _state.copyWith(dragging: value);

  int get _effectiveDurationUs => _mediaCoordinator.effectiveDurationUs;
  double get _timelineStartWidth => _playbackCoordinator.timelineStartWidth;
  int get _resolvedLoopStartUs => _playbackCoordinator.resolvedLoopStartUs;
  int get _resolvedLoopEndUs => _playbackCoordinator.resolvedLoopEndUs;
  List<int> get _loopMarkerPtsUs => _playbackCoordinator.loopMarkerPtsUs;

  // -- Build --

  @override
  Widget build(BuildContext context) {
    return MainWindowView(
      dragging: _dragging,
      onFilesDropped: (paths) {
        setState(() => _dragging = false);
        unawaited(_mediaCoordinator.loadMediaPaths(paths));
      },
      onDragEntered: () {
        if (!_dragging) setState(() => _dragging = true);
      },
      onDragExited: () {
        if (_dragging) setState(() => _dragging = false);
      },
      viewMode: _layout.mode,
      onViewModeChanged: (mode) {
        setState(() => _layout = _layout.copyWith(mode: mode));
        _layoutCoordinator.markLayoutDirty();
      },
      onAddMedia: _mediaCoordinator.openFile,
      onAnalysis: _analysisCoordinator.triggerAnalysis,
      onProfiler: () => WindowManager.showStatsWindow(),
      onSettings: () => WindowManager.showSettingsWindow(),
      viewModeEnabled: _textureId != null,
      analysisEnabled: _trackManager.count > 0,
      textureId: _textureId,
      viewportState: _viewportState,
      layout: _layout,
      onPan: _layoutCoordinator.onPan,
      onSplit: _layoutCoordinator.onSplit,
      onZoom: _layoutCoordinator.onZoom,
      onPointerButton: _layoutCoordinator.onPointerButton,
      onResize: _layoutCoordinator.onViewportResize,
      trackManager: _trackManager,
      onMediaSwapped: _mediaCoordinator.onMediaSwapped,
      onRemoveTrack: _mediaCoordinator.removeTrack,
      timelineSliderKey: _timelineSliderKey,
      timelineStartWidth: _timelineStartWidth,
      onZoomChanged: _layoutCoordinator.onZoomComboChanged,
      isPlaying: _isPlaying,
      onTogglePlay: _playbackCoordinator.togglePlayPause,
      onStepForward: () => _controller.stepForward(),
      onStepBackward: () => _controller.stepBackward(),
      currentPtsUs: _currentPtsUs,
      durationUs: _mediaCoordinator.effectiveDurationUs,
      onSeek: _playbackCoordinator.seekTo,
      onSliderHover: _playbackCoordinator.onSliderHover,
      markerUs: _loopMarkerPtsUs,
      seekMinUs: _loopRangeEnabled ? _resolvedLoopStartUs : null,
      seekMaxUs: _loopRangeEnabled ? _resolvedLoopEndUs : null,
      loopRangeBarKey: _loopRangeBarKey,
      loopRangeEnabled: _loopRangeEnabled,
      loopStartUs: _resolvedLoopStartUs,
      loopEndUs: _resolvedLoopEndUs,
      onLoopRangeEnabledChanged: (enabled) =>
          unawaited(_playbackCoordinator.setLoopRangeEnabled(enabled)),
      onLoopRangeChanged: (startUs, endUs) =>
          unawaited(_playbackCoordinator.setLoopRange(startUs, endUs)),
      onLoopRangeChangeEnd: _loopRangeEnabled
          ? (handle) => unawaited(
              _playbackCoordinator.setLoopRange(
                _resolvedLoopStartUs,
                _resolvedLoopEndUs,
                seekToStart: handle == LoopRangeHandle.start,
              ),
            )
          : null,
      onReorder: _trackManager.moveTrack,
      onOffsetChanged: _mediaCoordinator.onOffsetChanged,
      syncOffsets: _syncOffsets,
      hoverPtsUs: _hoverPtsUs,
      sliderHovering: _sliderHovering,
      controlsWidth: _timelineControlsWidth,
      onControlsWidthChanged: _setTimelineControlsWidth,
    );
  }
}
