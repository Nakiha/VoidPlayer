import 'dart:async';

import 'package:flutter/material.dart';

import '../../actions/action_registry.dart';
import '../../analysis/analysis_manager.dart';
import '../../analysis/analysis_toolbar_data_source.dart';
import '../../automation/test_runner.dart';
import '../../automation/ui_automation_bridge.dart';
import '../../config/app_config.dart';
import '../../config/app_settings_repository.dart';
import '../../marks/quick_mark_persistence.dart';
import '../../platform/analysis_process_host.dart';
import '../../platform/main_window_platform.dart';
import '../../platform/native_file_picker.dart';
import '../../platform/platform_capabilities.dart';
import '../../preferences/app_config_playback_preferences.dart';
import '../../preferences/playback_preferences.dart';
import '../../session/playback_session.dart';
import '../../startup_options.dart';
import '../../track_manager.dart';
import '../../utils/async_guard.dart';
import '../../video_renderer_controller.dart';
import '../../viewport/viewport_display_state.dart';
import '../../widgets/analysis_overlay_controls.dart';
import '../../widgets/loop_range_bar.dart';
import 'main_window_actions.dart';
import 'main_window_analysis.dart';
import 'main_window_fullscreen.dart';
import 'main_window_layout.dart';
import 'main_window_media.dart';
import 'main_window_media_lifecycle.dart';
import 'main_window_playback.dart';
import 'main_window_quick_marks.dart';
import 'main_window_state.dart';
import 'main_window_test_hooks.dart';
import 'main_window_timeline_metrics.dart';
import 'main_window_view_handles.dart';
import 'main_window_view_model.dart';
import 'main_window_view_model_factory.dart';

part 'main_window_action_binder.dart';

class MainWindowController {
  final ActionRegistry actionRegistry;
  final TickerProvider vsync;
  final StartupOptions startupOptions;
  final bool Function() mounted;
  final MainWindowPlatform platformWindow;
  final AnalysisProcessHost analysisProcesses;
  final PlatformCapabilities platformCapabilities;
  final NativeFilePicker nativeFilePicker;
  final AnalysisGenerationService analysisGeneration;
  final AnalysisToolbarDataSource analysisToolbarDataSource;
  final AppSettingsRepository appSettings;
  final PlaybackPreferences playbackPreferences;
  final QuickMarkRepository quickMarkRepository;
  final ValueChanged<int>? onDuplicateMediaSkipped;
  final void Function(String operation, Object error)? onUserActionFailed;

  final NativePlayerController player = NativePlayerController();
  final TrackManager trackManager = TrackManager();
  final MainWindowStateStore stateStore = MainWindowStateStore();
  final PlaybackSession _session = const PlaybackSession.normal();
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
  final GlobalKey controlsBarKey = GlobalKey();
  final GlobalKey analysisOverlayButtonKey = GlobalKey();
  final GlobalKey viewportKey = GlobalKey();
  final GlobalKey fullFrameCaptureKey = GlobalKey();
  late final MainWindowViewHandles viewHandles = MainWindowViewHandles(
    fullFrameCaptureKey: fullFrameCaptureKey,
    viewportKey: viewportKey,
    analysisOverlayButtonKey: analysisOverlayButtonKey,
    timelineSliderKey: timelineSliderKey,
    controlsBarKey: controlsBarKey,
    loopRangeBarKey: loopRangeBarKey,
    timelineHoverListenable: timelineHoverNotifier,
  );
  Future<void>? _shutdownFuture;

  late final MainWindowAnalysisCoordinator analysisCoordinator;
  late final MainWindowTestHarness testHarness;
  late final MainWindowLayoutCoordinator layoutCoordinator;
  late final MainWindowFullScreenCoordinator fullScreenCoordinator;
  late final MainWindowQuickMarkCoordinator quickMarkCoordinator;
  late final MainWindowMediaLifecycle mediaLifecycle;
  late final MainWindowMediaCoordinator mediaCoordinator;
  late final MainWindowPlaybackCoordinator playbackCoordinator;
  late final MainWindowActionCoordinator actionCoordinator;
  late final MainWindowViewActions _viewActions = _createViewActions();
  late final Listenable _listenable = stateStore;

  MainWindowController({
    required this.actionRegistry,
    required this.vsync,
    required this.startupOptions,
    required this.mounted,
    MainWindowPlatform? platformWindow,
    AnalysisProcessHost? analysisProcesses,
    this.platformCapabilities = PlatformCapabilities.windows,
    NativeFilePicker? nativeFilePicker,
    AnalysisGenerationService? analysisGeneration,
    AnalysisToolbarDataSource? analysisToolbarDataSource,
    AppSettingsRepository? appSettings,
    PlaybackPreferences? playbackPreferences,
    QuickMarkRepository? quickMarkRepository,
    this.onDuplicateMediaSkipped,
    this.onUserActionFailed,
  }) : platformWindow =
           platformWindow ?? const WindowManagerMainWindowPlatform(),
       analysisProcesses =
           analysisProcesses ?? UnsupportedAnalysisProcessHost(),
       nativeFilePicker =
           nativeFilePicker ?? const MethodChannelNativeFilePicker(),
       analysisGeneration = analysisGeneration ?? AnalysisManager.instance,
       appSettings =
           appSettings ?? AppConfigSettingsRepository(AppConfig.instance),
       analysisToolbarDataSource =
           analysisToolbarDataSource ??
           DefaultAnalysisToolbarDataSource(
             analysisManager: AnalysisManager.instance,
             settings:
                 appSettings ?? AppConfigSettingsRepository(AppConfig.instance),
           ),
       playbackPreferences =
           playbackPreferences ??
           AppConfigPlaybackPreferences(
             appSettings ?? AppConfigSettingsRepository(AppConfig.instance),
           ),
       quickMarkRepository =
           quickMarkRepository ?? const NoopQuickMarkRepository() {
    _initCoordinators();
    stateStore.addListener(_onStateChanged);
  }

  Listenable get listenable => _listenable;

  void start({String? testScriptPath}) {
    trackManager.addListener(_onTrackManagerChanged);
    actionCoordinator.bind();
    playbackCoordinator.startPolling();
    _maybeStartTestRunner(testScriptPath);
  }

  void dispose() {
    fireAndLog('dispose main window controller', closeGracefully());
  }

  Future<void> closeGracefully() {
    final existing = _shutdownFuture;
    if (existing != null) return existing;
    final future = _closeGracefullyImpl();
    _shutdownFuture = future;
    return future;
  }

  Future<void> _closeGracefullyImpl() async {
    stateStore.removeListener(_onStateChanged);
    actionCoordinator.dispose();
    playbackCoordinator.dispose();
    fullScreenCoordinator.dispose();
    await quickMarkCoordinator.closeGracefully();
    mediaCoordinator.dispose();
    layoutCoordinator.dispose();
    timelineHoverNotifier.dispose();
    try {
      await Future.wait([
        analysisCoordinator.dispose(),
        player.dispose(),
      ], eagerError: false);
    } finally {
      trackManager.dispose();
      stateStore.dispose();
    }
  }

  void setViewportBackgroundColor(Color color) {
    fireAndLog(
      'set viewport background color',
      player.setViewportBackgroundColor(color.toARGB32()),
    );
  }

  void setAnalysisAccentColor(Color color) {
    analysisCoordinator.publishAccentColor(color.toARGB32());
  }

  Future<void> _runUserAction(
    String operation,
    FutureOr<void> Function() action,
  ) {
    return runGuardedAction(
      operation,
      action,
      onError: (error, _) {
        if (!mounted()) return;
        onUserActionFailed?.call(operation, error);
      },
    );
  }

  void _fireUserAction(String operation, FutureOr<void> Function() action) {
    fireGuardedAction(
      operation,
      action,
      onError: (error, _) {
        if (!mounted()) return;
        onUserActionFailed?.call(operation, error);
      },
    );
  }

  void _onStateChanged() {
    quickMarkCoordinator.handleStateChanged();
  }

  MainWindowViewModel get viewModel {
    final markView = quickMarkCoordinator.view;
    return MainWindowViewModelFactory.build(
      session: _session,
      layout: _layout,
      textureId: _textureId,
      viewportState: _viewportState,
      tracks: trackManager.entries,
      markView: markView,
      quickMarkDraft: quickMarkCoordinator.draft,
      quickMarkThumbnails: quickMarkCoordinator.thumbnails,
      currentPtsUs: _currentPtsUs,
      platformCapabilities: platformCapabilities,
      syncOffsets: _syncOffsets,
      audibleTrackFileId: _audibleTrackFileId,
      performanceAlertPolicy: _performanceAlertPolicy,
      analysisDataSource: analysisToolbarDataSource,
      timelineStartWidth: _timelineStartWidth,
      isPlaying: _isPlaying,
      durationUs: timelineMetrics.effectiveDurationUs,
      markerUs: _loopMarkerPtsUs,
      seekMinUs: _loopRangeEnabled ? _resolvedLoopStartUs : null,
      seekMaxUs: _loopRangeEnabled ? _resolvedLoopEndUs : null,
      loopRangeEnabled: _loopRangeEnabled,
      loopStartUs: _resolvedLoopStartUs,
      loopEndUs: _resolvedLoopEndUs,
      controlsWidth: _timelineControlsWidth,
      dragging: _dragging,
      mediaInfoVisible: _mediaInfoVisible,
      profilerVisible: _profilerVisible,
      settingsVisible: _settingsVisible,
      analysisOverlayControlsVisible: _analysisOverlayControlsVisible,
      marksSidebarVisible: _marksSidebarVisible,
      marksSidebarWidth: _marksSidebarWidth,
      fullScreen: _fullScreen,
      fullScreenControlsVisible: _fullScreenControlsVisible,
    );
  }

  MainWindowViewActions get viewActions => _viewActions;

  void _toggleTrackAudio(int fileId) {
    final next = _audibleTrackFileId == fileId ? null : fileId;
    stateStore.setAudibleTrackFileId(next);
    fireAndLog('set audible track', player.setAudibleTrack(next));
  }

  void _toggleMarksSidebar() {
    _setMarksSidebarVisible(!_marksSidebarVisible);
  }

  void _setMarksSidebarVisible(bool visible) {
    if (_marksSidebarVisible == visible) return;
    final viewportDelta = visible ? -_marksSidebarWidth : _marksSidebarWidth;
    layoutCoordinator.requestPreemptViewportLogicalSizeDelta(
      widthDelta: viewportDelta,
    );
    stateStore.setMarksSidebarVisible(visible);
  }

  void _setAnalysisOverlayControlsVisible(bool visible) {
    if (_analysisOverlayControlsVisible == visible) return;
    if (!_fullScreen) {
      final viewportDelta = visible
          ? -AnalysisOverlayStrip.height
          : AnalysisOverlayStrip.height;
      layoutCoordinator.requestPreemptViewportLogicalSizeDelta(
        heightDelta: viewportDelta,
      );
    }
    stateStore.setAnalysisOverlayControlsVisible(visible);
  }

  void _setMarksSidebarWidth(double width) {
    final next = width
        .clamp(kMinMarksSidebarWidth, kMaxMarksSidebarWidth)
        .toDouble();
    final delta = next - _marksSidebarWidth;
    if (delta == 0) return;
    stateStore.setMarksSidebarWidth(next);
  }

  void _initCoordinators() {
    layoutCoordinator = MainWindowLayoutCoordinator(
      vsync: vsync,
      controller: player,
      stateStore: stateStore,
      trackManager: trackManager,
      mounted: mounted,
    );
    stateStore.setLayout(
      _layout.copyWith(
        pixelSizeMode: playbackPreferences.viewportPixelSizeMode.layoutValue,
      ),
    );
    stateStore.setPerformanceAlertPolicy(
      playbackPreferences.performanceAlertPolicy,
    );
    fullScreenCoordinator = MainWindowFullScreenCoordinator(
      platformWindow: platformWindow,
      layoutCoordinator: layoutCoordinator,
      stateStore: stateStore,
      viewportKey: viewportKey,
      mounted: mounted,
    );
    analysisCoordinator = MainWindowAnalysisCoordinator(
      trackManager: trackManager,
      analysisProcesses: analysisProcesses,
      analysisGeneration: analysisGeneration,
      analysisOverlaysEnabled: platformCapabilities.analysisOverlays,
      presentedFrameProvider: player.currentPresentedFrame,
      onOverlayStateChanged: _requestAnalysisOverlayRedraw,
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
      onSeekSettled: (_) => analysisCoordinator.refreshOverlayForCurrentFrame(),
      onSeekPreviewPresented:
          ({required trackFileId, required ptsUs, required dtsUs}) =>
              analysisCoordinator.refreshOverlayForPresentedFrame(
                trackFileId: trackFileId,
                ptsUs: ptsUs,
                dtsUs: dtsUs,
              ),
    );
    quickMarkCoordinator = MainWindowQuickMarkCoordinator(
      player: player,
      trackManager: trackManager,
      stateStore: stateStore,
      layoutCoordinator: layoutCoordinator,
      playbackCoordinator: playbackCoordinator,
      repository: quickMarkRepository,
      mounted: mounted,
      shuttingDown: () => _shutdownFuture != null,
    );
    mediaLifecycle = MainWindowMediaLifecycle(
      stateStore: stateStore,
      trackManager: trackManager,
      playbackCoordinator: playbackCoordinator,
      requestFullScreen: fullScreenCoordinator.request,
    );
    mediaCoordinator = MainWindowMediaCoordinator(
      controller: player,
      trackManager: trackManager,
      layoutCoordinator: layoutCoordinator,
      stateStore: stateStore,
      timelineMetrics: timelineMetrics,
      lifecycle: mediaLifecycle,
      playbackPreferences: playbackPreferences,
      nativeFilePicker: nativeFilePicker,
      appSettings: appSettings,
      mounted: mounted,
      onDuplicateMediaSkipped: onDuplicateMediaSkipped,
    );
    testHarness = MainWindowTestHarness(
      viewportKey: viewportKey,
      timelineSliderKey: timelineSliderKey,
      controlsBarKey: controlsBarKey,
      analysisOverlayButtonKey: analysisOverlayButtonKey,
      fullFrameCaptureKey: fullFrameCaptureKey,
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
      showMediaInfoOverlay: () {
        if (!trackManager.isEmpty) stateStore.setMediaInfoVisible(true);
      },
      showProfilerOverlay: () => stateStore.setProfilerVisible(true),
      showSettingsDialog: () => stateStore.setSettingsVisible(true),
      toggleFullScreen: fullScreenCoordinator.toggle,
      exitFullScreen: fullScreenCoordinator.exit,
      capabilities: () => _capabilities,
      removeTrack: _removeTrack,
    );
  }

  void _setViewportPixelSizeMode(ViewportPixelSizeMode mode) {
    layoutCoordinator.setPixelSizeMode(mode.layoutValue);
  }

  void _setPerformanceAlertPolicy(PerformanceAlertPolicy policy) {
    stateStore.setPerformanceAlertPolicy(policy);
  }

  void _requestAnalysisOverlayRedraw() {
    if (!mounted()) return;
    fireAndLog('redraw analysis overlay', player.applyLayout(_layout));
  }

  void _maybeStartTestRunner(String? path) {
    if (path == null) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      TestRunner(
        scriptPath: path,
        automation: UiAutomationBridge(
          controller: player,
          analysisProcesses: analysisProcesses,
          testHarness: testHarness,
          effectiveDurationUs: () => timelineMetrics.effectiveDurationUs,
          toggleAnalysisOverlayForSlot:
              analysisCoordinator.toggleOverlayForSlot,
          toggleAnalysisOverlayPanel: analysisCoordinator.toggleOverlayPanel,
          generateAnalysisCacheForSlot:
              analysisCoordinator.ensureGeneratedForSlot,
          setAnalysisOverlayType: (type) {
            analysisCoordinator.updateOverlayConfig(
              analysisGeneration.overlayConfig.withTypeDefaults(type),
            );
          },
          setAnalysisOverlayLayers: (layers) {
            analysisCoordinator.updateOverlayConfig(
              analysisGeneration.overlayConfig.copyWith(layers: layers),
            );
          },
          setAnalysisOverlayOpacity: (opacity) {
            analysisCoordinator.updateOverlayConfig(
              analysisGeneration.overlayConfig.copyWith(opacity: opacity),
            );
          },
          actionRegistry: actionRegistry,
        ),
      ).run();
    });
  }

  void _onTrackManagerChanged() {
    stateStore.setLayout(_layout.copyWith(order: trackManager.order));
    layoutCoordinator.markLayoutDirty();
    quickMarkCoordinator.reconcilePersistence();
    fireAndLog(
      'publish analysis track snapshot',
      analysisCoordinator.publishTrackSnapshot(),
    );
    fireAndLog(
      'sync analysis overlay tracks',
      analysisCoordinator.syncOverlayPanelTracks(),
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
  bool get _mediaInfoVisible => _state.mediaInfoVisible;
  bool get _profilerVisible => _state.profilerVisible;
  bool get _settingsVisible => _state.settingsVisible;
  bool get _analysisOverlayControlsVisible =>
      _state.analysisOverlayControlsVisible;
  bool get _marksSidebarVisible => _state.marksSidebarVisible;
  double get _marksSidebarWidth => _state.marksSidebarWidth;
  bool get _fullScreen => _state.fullScreen;
  bool get _fullScreenControlsVisible => _state.fullScreenControlsVisible;
  int? get _audibleTrackFileId => _state.audibleTrackFileId;
  PerformanceAlertPolicy get _performanceAlertPolicy =>
      _state.performanceAlertPolicy;
  SessionCapabilities get _capabilities => _session.capabilities;
  double get _timelineStartWidth => playbackCoordinator.timelineStartWidth;
  int get _resolvedLoopStartUs => playbackCoordinator.resolvedLoopStartUs;
  int get _resolvedLoopEndUs => playbackCoordinator.resolvedLoopEndUs;
  List<int> get _loopMarkerPtsUs => playbackCoordinator.loopMarkerPtsUs;

  Future<void> _removeTrack(int fileId) async {
    await mediaCoordinator.removeTrack(fileId);
    if (trackManager.entries.any((entry) => entry.fileId == fileId)) return;
    quickMarkCoordinator.deleteForFileId(fileId);
  }
}
