import 'dart:async';

import 'package:flutter/material.dart';

import '../../actions/action_registry.dart';
import '../../analysis/analysis_manager.dart';
import '../../automation/test_runner.dart';
import '../../automation/ui_automation_bridge.dart';
import '../../config/app_config.dart';
import '../../config/app_settings_repository.dart';
import '../../preferences/app_config_playback_preferences.dart';
import '../../preferences/playback_preferences.dart';
import '../../startup_options.dart';
import '../../track_manager.dart';
import '../../utils/async_guard.dart';
import '../../video_renderer_controller.dart';
import '../../viewport/viewport_display_state.dart';
import '../../widgets/loop_range_bar.dart';
import '../window_manager.dart' as app_window;
import 'main_window_actions.dart';
import 'main_window_analysis.dart';
import 'main_window_layout.dart';
import 'main_window_media.dart';
import 'main_window_media_lifecycle.dart';
import 'main_window_platform.dart';
import 'main_window_playback.dart';
import 'main_window_state.dart';
import 'main_window_test_hooks.dart';
import 'main_window_timeline_metrics.dart';
import 'main_window_view_model.dart';

class MainWindowController {
  final ActionRegistry actionRegistry;
  final TickerProvider vsync;
  final StartupOptions startupOptions;
  final bool Function() mounted;
  final MainWindowPlatform platformWindow;
  final app_window.AnalysisProcessManager analysisProcesses;
  final AnalysisGenerationService analysisGeneration;
  final PlaybackPreferences playbackPreferences;

  final NativePlayerController player = NativePlayerController();
  final TrackManager trackManager = TrackManager();
  final MainWindowStateStore stateStore = MainWindowStateStore();
  late final MainWindowTimelineMetrics timelineMetrics =
      MainWindowTimelineMetrics(
        stateStore: stateStore,
        trackManager: trackManager,
      );
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
  late final MainWindowMediaLifecycle mediaLifecycle;
  late final MainWindowMediaCoordinator mediaCoordinator;
  late final MainWindowPlaybackCoordinator playbackCoordinator;
  late final MainWindowActionCoordinator actionCoordinator;
  late final MainWindowViewActions _viewActions = _createViewActions();

  MainWindowController({
    required this.actionRegistry,
    required this.vsync,
    required this.startupOptions,
    required this.mounted,
    MainWindowPlatform? platformWindow,
    app_window.AnalysisProcessManager? analysisProcesses,
    AnalysisGenerationService? analysisGeneration,
    PlaybackPreferences? playbackPreferences,
  }) : platformWindow = platformWindow ?? const WindowsMainWindowPlatform(),
       analysisProcesses =
           analysisProcesses ?? app_window.WindowManager.analysisProcesses,
       analysisGeneration = analysisGeneration ?? AnalysisManager.instance,
       playbackPreferences =
           playbackPreferences ??
           AppConfigPlaybackPreferences(
             AppConfigSettingsRepository(AppConfig.instance),
           ) {
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
      viewport: MainWindowViewportVm(
        viewMode: _layout.mode,
        viewModeEnabled: _textureId != null,
        textureId: _textureId,
        viewportState: _viewportState,
        layout: _layout,
        viewportKey: viewportKey,
      ),
      media: MainWindowMediaVm(
        analysisEnabled: trackManager.count > 0,
        tracks: trackManager.entries,
        syncOffsets: _syncOffsets,
        audibleTrackFileId: _audibleTrackFileId,
      ),
      playback: MainWindowPlaybackVm(
        timelineSliderKey: timelineSliderKey,
        timelineStartWidth: _timelineStartWidth,
        isPlaying: _isPlaying,
        currentPtsUs: _currentPtsUs,
        durationUs: timelineMetrics.effectiveDurationUs,
        markerUs: _loopMarkerPtsUs,
        seekMinUs: _loopRangeEnabled ? _resolvedLoopStartUs : null,
        seekMaxUs: _loopRangeEnabled ? _resolvedLoopEndUs : null,
        loopRangeBarKey: loopRangeBarKey,
        loopRangeEnabled: _loopRangeEnabled,
        loopStartUs: _resolvedLoopStartUs,
        loopEndUs: _resolvedLoopEndUs,
        timelineHoverListenable: timelineHoverNotifier,
        controlsWidth: _timelineControlsWidth,
      ),
      overlays: MainWindowOverlayVm(
        dragging: _dragging,
        profilerVisible: _profilerVisible,
        settingsVisible: _settingsVisible,
        fullScreen: _fullScreen,
        fullScreenControlsVisible: _fullScreenControlsVisible,
      ),
    );
  }

  MainWindowViewActions get viewActions => _viewActions;

  MainWindowViewActions _createViewActions() {
    return MainWindowViewActions(
      drop: MainWindowDropActions(
        filesDropped: (paths) {
          stateStore.setDragging(false);
          fireAndLog(
            'load dropped media',
            mediaCoordinator.loadMediaPaths(paths),
          );
        },
        dragEntered: () {
          if (!_dragging) stateStore.setDragging(true);
        },
        dragExited: () {
          if (_dragging) stateStore.setDragging(false);
        },
      ),
      toolbar: MainWindowToolbarActions(
        onViewModeChanged: (mode) {
          stateStore.setLayout(_layout.copyWith(mode: mode));
          layoutCoordinator.markLayoutDirty();
        },
        onAddMedia: mediaCoordinator.openFile,
        onAnalysis: analysisCoordinator.triggerAnalysis,
        onProfiler: () => stateStore.setProfilerVisible(!_profilerVisible),
        onSettings: () => stateStore.setSettingsVisible(!_settingsVisible),
      ),
      viewport: MainWindowViewportActions(
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
      ),
      mediaTimeline: MainWindowMediaTimelineActions(
        onMediaSwapped: mediaCoordinator.onMediaSwapped,
        onRemoveTrack: mediaCoordinator.removeTrack,
        onZoomChanged: layoutCoordinator.onZoomComboChanged,
        onToggleFullScreen: _toggleFullScreen,
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
      ),
      overlays: MainWindowOverlayActions(
        onCloseProfiler: () => stateStore.setProfilerVisible(false),
        onCloseSettings: () => stateStore.setSettingsVisible(false),
        onFullScreenPointerActivity: _showFullScreenControlsTemporarily,
        onFullScreenControlsHoverChanged: _setFullScreenControlsHovering,
      ),
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
      await platformWindow.setFullScreen(fullScreen);
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
    final bounds = await platformWindow.getBounds();
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
      stateStore: stateStore,
      trackManager: trackManager,
      mounted: mounted,
    );
    analysisCoordinator = MainWindowAnalysisCoordinator(
      trackManager: trackManager,
      analysisProcesses: analysisProcesses,
      analysisGeneration: analysisGeneration,
    );
    playbackCoordinator = MainWindowPlaybackCoordinator(
      controller: player,
      trackManager: trackManager,
      startupOptions: startupOptions,
      stateStore: stateStore,
      timelineHoverNotifier: timelineHoverNotifier,
      playbackPreferences: playbackPreferences,
      mounted: mounted,
      timelineMetrics: timelineMetrics,
    );
    mediaLifecycle = MainWindowMediaLifecycle(
      stateStore: stateStore,
      trackManager: trackManager,
      playbackCoordinator: playbackCoordinator,
      requestFullScreen: _requestFullScreen,
    );
    mediaCoordinator = MainWindowMediaCoordinator(
      controller: player,
      trackManager: trackManager,
      layoutCoordinator: layoutCoordinator,
      stateStore: stateStore,
      timelineMetrics: timelineMetrics,
      lifecycle: mediaLifecycle,
      mounted: mounted,
    );
    testHarness = MainWindowTestHarness(
      viewportKey: viewportKey,
      timelineSliderKey: timelineSliderKey,
      loopRangeBarKey: loopRangeBarKey,
      splitPosition: () => _layout.splitPos,
      timelineStartWidth: () => _timelineStartWidth,
      effectiveDurationUs: () => timelineMetrics.effectiveDurationUs,
      resolvedLoopStartUs: () => _resolvedLoopStartUs,
      resolvedLoopEndUs: () => _resolvedLoopEndUs,
    );
    actionCoordinator = MainWindowActionCoordinator(
      actionRegistry: actionRegistry,
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
      openNewWindow: () => stateStore.setProfilerVisible(true),
    );
  }

  void _maybeStartTestRunner(String? path) {
    if (path == null) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      TestRunner(
        scriptPath: path,
        automation: UiAutomationBridge(
          controller: player,
          analysisProcesses: analysisProcesses,
          actionRegistry: actionRegistry,
        ),
      ).run();
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

  MainWindowStateModel get _state => stateStore.value;

  int? get _textureId => _state.textureId;
  ViewportDisplayState get _viewportState => _state.viewportState;
  bool get _isPlaying => _state.isPlaying;
  int get _currentPtsUs => _state.currentPtsUs;
  LayoutState get _layout => _state.layout;
  Map<int, int> get _syncOffsets => _state.syncOffsets;
  double get _timelineControlsWidth => _state.timelineControlsWidth;
  bool get _loopRangeEnabled => _state.loopRangeEnabled;
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
