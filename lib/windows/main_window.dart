import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:path/path.dart' as p;
import '../app_log.dart';
import '../startup_options.dart';
import '../video_renderer_controller.dart';
import '../track_manager.dart';
import '../actions/test_runner.dart';
import 'window_manager.dart';
import '../analysis/analysis_manager.dart';
import '../widgets/loop_range_bar.dart';
import 'analysis_ipc.dart';
import 'main_window_actions.dart';
import 'main_window_state.dart' as main_state;
import 'main_window_test_hooks.dart' as main_hooks;
import 'main_window_view.dart';
import 'native_file_picker.dart';

part 'main_window_analysis.dart';
part 'main_window_layout.dart';
part 'main_window_media.dart';
part 'main_window_playback.dart';
part 'main_window_tracks.dart';

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
  static const double _trackDragHandleWidth = 28.0;
  static const double _trackDividerWidth = 1.0;
  static const Duration _viewportResizeDebounce = Duration(milliseconds: 80);

  final _controller = VideoRendererController();
  final _trackManager = TrackManager();
  final _analysisIpcServer = AnalysisIpcServer();
  final _analysisHashesByFileId = <int, String>{};
  final _timelineSliderKey = GlobalKey();
  final _loopRangeBarKey = GlobalKey();
  late final main_hooks.MainWindowTestHarness _testHarness;
  int _analysisSnapshotSerial = 0;

  main_state.MainWindowStateModel _state =
      const main_state.MainWindowStateModel();
  int _loopRangeSyncSerial = 0;

  // Polling
  Timer? _pollTimer;
  Timer? _loopBoundaryTimer;
  Ticker? _layoutTicker;
  bool _layoutDirty = false;

  bool _resizeDirty = false;
  bool _layoutFlushInProgress = false;
  Timer? _resizeDebounceTimer;

  @override
  void initState() {
    super.initState();
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
    _startPolling();
    _startLayoutTicker();
    _maybeStartTestRunner();
  }

  void _bindActions() {
    MainWindowActionBinder(
      togglePlayPause: _togglePlayPause,
      play: _play,
      pause: _pause,
      stepForward: _controller.stepForward,
      stepBackward: _controller.stepBackward,
      seekTo: _seekTo,
      clickTimelineFraction: _testHarness.clickTimelineFraction,
      setSpeed: _setSpeed,
      openFile: _openFile,
      addMediaByPath: _addMediaByPath,
      removeTrack: _onRemoveTrack,
      adjustTrackOffset: _onOffsetChanged,
      setLoopRangeEnabled: _setLoopRangeEnabled,
      isLoopRangeEnabled: () => _loopRangeEnabled,
      setLoopRange: _setLoopRange,
      dragLoopHandle: _testHarness.dragLoopHandle,
      toggleLayoutMode: _toggleLayoutMode,
      setLayoutMode: _setLayoutMode,
      setZoom: _setZoom,
      setSplitPos: _setSplitPos,
      panByDelta: _panByDelta,
      openNewWindow: WindowManager.showStatsWindow,
      openSettings: WindowManager.showSettingsWindow,
      openStats: WindowManager.showStatsWindow,
      openMemory: WindowManager.showMemoryWindow,
      runAnalysis: _triggerAnalysis,
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
    _pollTimer?.cancel();
    _loopBoundaryTimer?.cancel();
    _resizeDebounceTimer?.cancel();
    _layoutTicker?.dispose();
    unawaited(_analysisIpcServer.dispose());
    _trackManager.dispose();
    unawaited(_controller.dispose());
    super.dispose();
  }

  // -- TrackManager listener --

  void _onTrackManagerChanged() {
    setState(() {
      _layout = _layout.copyWith(order: _trackManager.order);
    });
    _markLayoutDirty();
    unawaited(_publishAnalysisTrackSnapshot());
  }

  void _setViewportState(int state) {
    setState(() => _viewportState = state);
  }

  void _setTextureId(int textureId) {
    setState(() => _textureId = textureId);
  }

  void _replaceLayout(LayoutState layout) {
    setState(() => _layout = layout);
  }

  void _updateLayout(LayoutState Function(LayoutState current) update) {
    setState(() => _layout = update(_layout));
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
      _loopRangeSyncSerial++;
      _loopStartUs = 0;
      _loopEndUs = 0;
    });
  }

  void _removeSyncOffset(int slot) {
    setState(() {
      _syncOffsets = Map.from(_syncOffsets)..remove(slot);
    });
  }

  void _setSyncOffset(int slot, int offsetUs) {
    setState(() {
      _syncOffsets = Map.from(_syncOffsets)..[slot] = offsetUs;
    });
  }

  void _setPlaying(bool playing) {
    setState(() => _isPlaying = playing);
  }

  void _setSeekPreview(int ptsUs) {
    setState(() {
      _currentPtsUs = ptsUs;
      _pendingSeekUs = ptsUs;
      _pendingSeekAt = DateTime.now();
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

  int get _viewportWidth => _state.viewportWidth;
  set _viewportWidth(int value) =>
      _state = _state.copyWith(viewportWidth: value);

  int get _viewportHeight => _state.viewportHeight;
  set _viewportHeight(int value) =>
      _state = _state.copyWith(viewportHeight: value);

  bool get _dragging => _state.dragging;
  set _dragging(bool value) => _state = _state.copyWith(dragging: value);

  // -- Build --

  @override
  Widget build(BuildContext context) {
    return MainWindowView(
      dragging: _dragging,
      onFilesDropped: (paths) {
        setState(() => _dragging = false);
        _loadMediaPaths(paths);
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
        _markLayoutDirty();
      },
      onAddMedia: _openFile,
      onAnalysis: _triggerAnalysis,
      onProfiler: () => WindowManager.showStatsWindow(),
      onSettings: () => WindowManager.showSettingsWindow(),
      viewModeEnabled: _textureId != null,
      analysisEnabled: _trackManager.count > 0,
      textureId: _textureId,
      viewportState: _viewportState,
      layout: _layout,
      onPan: _onPan,
      onSplit: _onSplit,
      onZoom: _onZoom,
      onPointerButton: _onPointerButton,
      onResize: _onViewportResize,
      trackManager: _trackManager,
      onMediaSwapped: _onMediaSwapped,
      onRemoveTrack: _onRemoveTrack,
      timelineSliderKey: _timelineSliderKey,
      timelineStartWidth: _timelineStartWidth,
      onZoomChanged: _onZoomComboChanged,
      isPlaying: _isPlaying,
      onTogglePlay: _togglePlayPause,
      onStepForward: () => _controller.stepForward(),
      onStepBackward: () => _controller.stepBackward(),
      currentPtsUs: _currentPtsUs,
      durationUs: _effectiveDurationUs,
      onSeek: _seekTo,
      onSliderHover: _onSliderHover,
      markerUs: _loopMarkerPtsUs,
      seekMinUs: _loopRangeEnabled ? _resolvedLoopStartUs : null,
      seekMaxUs: _loopRangeEnabled ? _resolvedLoopEndUs : null,
      loopRangeBarKey: _loopRangeBarKey,
      loopRangeEnabled: _loopRangeEnabled,
      loopStartUs: _resolvedLoopStartUs,
      loopEndUs: _resolvedLoopEndUs,
      onLoopRangeEnabledChanged: _setLoopRangeEnabled,
      onLoopRangeChanged: (startUs, endUs) => _setLoopRange(startUs, endUs),
      onLoopRangeChangeEnd: _loopRangeEnabled
          ? (handle) => _setLoopRange(
              _resolvedLoopStartUs,
              _resolvedLoopEndUs,
              seekToStart: handle == LoopRangeHandle.start,
            )
          : null,
      onReorder: _trackManager.moveTrack,
      onOffsetChanged: _onOffsetChanged,
      syncOffsets: _syncOffsets,
      hoverPtsUs: _hoverPtsUs,
      sliderHovering: _sliderHovering,
      controlsWidth: _timelineControlsWidth,
      onControlsWidthChanged: _setTimelineControlsWidth,
    );
  }
}
