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
  static const double _toolbarLogicalHeight = 40.0;
  static const double _mediaHeaderLogicalHeight = 32.0;
  static const double _controlsBarLogicalHeight = 40.0;
  static const double _loopRangeBarLogicalHeight = 40.0;

  final TickerProvider vsync;
  final StartupOptions startupOptions;
  final bool Function() mounted;

  final NativePlayerController player = NativePlayerController();
  final TrackManager trackManager = TrackManager();
  final MainWindowStateStore stateStore = MainWindowStateStore();
  final GlobalKey timelineSliderKey = GlobalKey();
  final GlobalKey loopRangeBarKey = GlobalKey();
  final GlobalKey viewportKey = GlobalKey();
  Timer? _fullScreenControlsTimer;

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
    _fullScreenControlsTimer?.cancel();
    actionCoordinator.dispose();
    playbackCoordinator.dispose();
    mediaCoordinator.dispose();
    layoutCoordinator.dispose();
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
      hoverPtsUs: _hoverPtsUs,
      sliderHovering: _sliderHovering,
      controlsWidth: _timelineControlsWidth,
      profilerVisible: _profilerVisible,
      settingsVisible: _settingsVisible,
      fullScreen: _fullScreen,
      fullScreenControlsVisible: _fullScreenControlsVisible,
      audibleTrackFileId: _audibleTrackFileId,
    );
  }

  MainWindowViewActions get viewActions {
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
      onResize: layoutCoordinator.onViewportResize,
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
      onLoopRangeChanged: (startUs, endUs) => fireAndLog(
        'set loop range',
        playbackCoordinator.setLoopRange(startUs, endUs),
      ),
      onLoopRangeChangeEnd: _loopRangeEnabled
          ? (handle) => fireAndLog(
              'finish loop range change',
              playbackCoordinator.setLoopRange(
                _resolvedLoopStartUs,
                _resolvedLoopEndUs,
                seekToStart: handle == LoopRangeHandle.start,
              ),
            )
          : null,
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
    fireAndLog('toggle full screen', _setFullScreen(!_fullScreen));
  }

  void _exitFullScreen() {
    if (!_fullScreen) return;
    fireAndLog('exit full screen', _setFullScreen(false));
  }

  Future<void> _setFullScreen(bool fullScreen) async {
    _fullScreenControlsTimer?.cancel();
    final initialBounds = await windowManager.getBounds();
    await windowManager.setFullScreen(fullScreen);
    if (!mounted()) return;
    await _waitForWindowBoundsToSettle(
      initialBounds: initialBounds,
      fullScreen: fullScreen,
    );
    if (!mounted()) return;
    final settledBounds = await windowManager.getBounds();
    await _preemptFullScreenViewportResize(
      windowBounds: settledBounds,
      fullScreen: fullScreen,
    );
    if (!mounted()) return;
    stateStore.setFullScreen(fullScreen);
    if (fullScreen) {
      _scheduleFullScreenControlsHide();
    }
  }

  Future<void> _waitForWindowBoundsToSettle({
    required Rect initialBounds,
    required bool fullScreen,
  }) async {
    final stopwatch = Stopwatch()..start();
    var previous = initialBounds;
    var stableSamples = 0;

    while (stopwatch.elapsed < const Duration(milliseconds: 500)) {
      await Future<void>.delayed(const Duration(milliseconds: 16));
      final current = await windowManager.getBounds();
      final fullScreenApplied = await windowManager.isFullScreen();
      final boundsChanged = !_sameWindowBounds(current, initialBounds);
      final boundsStable = _sameWindowBounds(current, previous);

      stableSamples = boundsStable ? stableSamples + 1 : 0;
      previous = current;

      if (fullScreenApplied == fullScreen &&
          (boundsChanged ||
              stopwatch.elapsed > const Duration(milliseconds: 80)) &&
          stableSamples >= 2) {
        break;
      }
    }
  }

  bool _sameWindowBounds(Rect a, Rect b) {
    return (a.left - b.left).abs() < 0.5 &&
        (a.top - b.top).abs() < 0.5 &&
        (a.width - b.width).abs() < 0.5 &&
        (a.height - b.height).abs() < 0.5;
  }

  Future<void> _preemptFullScreenViewportResize({
    required Rect windowBounds,
    required bool fullScreen,
  }) async {
    final dpr = layoutCoordinator.viewportDevicePixelRatio;
    if (dpr <= 0) return;

    final viewportLogicalWidth = windowBounds.width;
    final viewportLogicalHeight = fullScreen
        ? windowBounds.height
        : (windowBounds.height - _nonFullScreenChromeLogicalHeight).clamp(
            1.0,
            double.infinity,
          );
    await layoutCoordinator.preemptViewportResize(
      width: (viewportLogicalWidth * dpr).round(),
      height: (viewportLogicalHeight * dpr).round(),
    );
  }

  double get _nonFullScreenChromeLogicalHeight {
    if (trackManager.count <= 0) return _toolbarLogicalHeight;
    return _toolbarLogicalHeight +
        _mediaHeaderLogicalHeight +
        _controlsBarLogicalHeight +
        _loopRangeBarLogicalHeight +
        trackManager.count *
            MainWindowLayoutCoordinator.timelineTrackRowLogicalHeight;
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
      hoverPtsUs: () => _hoverPtsUs,
      sliderHovering: () => _sliderHovering,
      setSliderHoverState: stateStore.setSliderHover,
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
      fireAndLog(
        'exit full screen after last track removed',
        _setFullScreen(false),
      );
    }
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
