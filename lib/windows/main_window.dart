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
  late final MainWindowActionCoordinator _actionCoordinator;
  late final main_state.MainWindowStateStore _stateStore;

  @override
  void initState() {
    super.initState();
    _stateStore = main_state.MainWindowStateStore();
    _layoutCoordinator = MainWindowLayoutCoordinator(
      vsync: this,
      controller: _controller,
      mounted: () => mounted,
      textureId: () => _textureId,
      layout: () => _layout,
      setLayout: _stateStore.setLayout,
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
      setPlaying: _stateStore.setPlaying,
      playbackSpeed: () => _playbackSpeed,
      setPlaybackSpeed: _stateStore.setPlaybackSpeed,
      currentPtsUs: () => _currentPtsUs,
      durationUs: () => _durationUs,
      pendingSeekUs: () => _pendingSeekUs,
      pendingSeekAt: () => _pendingSeekAt,
      setSeekPreview: _stateStore.setSeekPreview,
      setPendingSeek: _stateStore.setPendingSeek,
      setPolledPlaybackState: _stateStore.setPolledPlaybackState,
      loopRangeEnabled: () => _loopRangeEnabled,
      setLoopRangeEnabledState: _stateStore.setLoopRangeEnabled,
      nativeLoopRangeSynced: () => _nativeLoopRangeSynced,
      setNativeLoopRangeSynced: _stateStore.setNativeLoopRangeSynced,
      startupLoopRangeApplied: () => _startupLoopRangeApplied,
      setStartupLoopRangeApplied: _stateStore.setStartupLoopRangeApplied,
      loopStartUs: () => _loopStartUs,
      loopEndUs: () => _loopEndUs,
      setLoopRangeState: _stateStore.setLoopRange,
      hoverPtsUs: () => _hoverPtsUs,
      sliderHovering: () => _sliderHovering,
      setSliderHoverState: _stateStore.setSliderHover,
    );
    _mediaCoordinator = MainWindowMediaCoordinator(
      controller: _controller,
      trackManager: _trackManager,
      layoutCoordinator: _layoutCoordinator,
      mounted: () => mounted,
      textureId: () => _textureId,
      setViewportState: _stateStore.setViewportState,
      setTextureId: _stateStore.setTextureId,
      setLayout: _stateStore.setLayout,
      syncOffsets: () => _syncOffsets,
      setSyncOffsets: _stateStore.setSyncOffsets,
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
    _actionCoordinator = MainWindowActionCoordinator(
      controller: _controller,
      playbackCoordinator: _playbackCoordinator,
      mediaCoordinator: _mediaCoordinator,
      layoutCoordinator: _layoutCoordinator,
      analysisCoordinator: _analysisCoordinator,
      testHarness: _testHarness,
      isLoopRangeEnabled: () => _loopRangeEnabled,
    );
    _trackManager.addListener(_onTrackManagerChanged);
    _actionCoordinator.bind();
    _playbackCoordinator.startPolling();
    _maybeStartTestRunner();
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
    _actionCoordinator.dispose();
    _playbackCoordinator.dispose();
    _layoutCoordinator.dispose();
    _stateStore.dispose();
    unawaited(_analysisCoordinator.dispose());
    _trackManager.dispose();
    unawaited(_controller.dispose());
    super.dispose();
  }

  // -- TrackManager listener --

  void _onTrackManagerChanged() {
    _stateStore.setLayout(_layout.copyWith(order: _trackManager.order));
    _layoutCoordinator.markLayoutDirty();
    unawaited(_analysisCoordinator.publishTrackSnapshot());
  }

  void _resetAfterLastTrackRemoved() {
    _trackManager.clear();
    _stateStore.resetAfterLastTrackRemoved();
    _playbackCoordinator.invalidateLoopRangeSync();
  }

  main_state.MainWindowStateModel get _state => _stateStore.value;

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
  int get _effectiveDurationUs => _mediaCoordinator.effectiveDurationUs;
  double get _timelineStartWidth => _playbackCoordinator.timelineStartWidth;
  int get _resolvedLoopStartUs => _playbackCoordinator.resolvedLoopStartUs;
  int get _resolvedLoopEndUs => _playbackCoordinator.resolvedLoopEndUs;
  List<int> get _loopMarkerPtsUs => _playbackCoordinator.loopMarkerPtsUs;

  // -- Build --

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: _stateStore,
      builder: (context, _) => _buildView(context),
    );
  }

  Widget _buildView(BuildContext context) {
    return MainWindowView(
      dragging: _dragging,
      onFilesDropped: (paths) {
        _stateStore.setDragging(false);
        unawaited(_mediaCoordinator.loadMediaPaths(paths));
      },
      onDragEntered: () {
        if (!_dragging) _stateStore.setDragging(true);
      },
      onDragExited: () {
        if (_dragging) _stateStore.setDragging(false);
      },
      viewMode: _layout.mode,
      onViewModeChanged: (mode) {
        _stateStore.setLayout(_layout.copyWith(mode: mode));
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
      onControlsWidthChanged: _stateStore.setTimelineControlsWidth,
    );
  }
}
