import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';

import '../actions/action_registry.dart';
import '../agent/agent_protocol_server.dart';
import '../analysis/analysis_manager.dart';
import '../analysis/analysis_toolbar_data_source.dart';
import '../app_log.dart';
import '../automation/main_window_harness.dart';
import '../automation/test_runner.dart';
import '../automation/ui_automation_bridge.dart';
import '../config/app_config.dart';
import '../config/app_settings_repository.dart';
import '../marks/quick_mark_persistence.dart';
import '../platform/analysis_process_host.dart';
import '../platform/main_window_platform.dart';
import '../platform/native_file_picker.dart';
import '../platform/platform_capabilities.dart';
import '../preferences/app_config_playback_preferences.dart';
import '../preferences/playback_preferences.dart';
import '../session/playback_session.dart';
import '../startup_options.dart';
import '../track_manager.dart';
import '../utils/async_guard.dart';
import '../video_renderer_controller.dart';
import '../viewport/viewport_display_state.dart';
import '../widgets/loop_range_bar.dart';
import 'main_window_actions.dart';
import 'main_window_agent.dart';
import 'main_window_analysis.dart';
import 'main_window_fullscreen.dart';
import 'main_window_layout.dart';
import 'main_window_media.dart';
import 'main_window_media_lifecycle.dart';
import 'main_window_playback.dart';
import 'main_window_quick_marks.dart';
import 'main_window_state.dart';
import 'main_window_timeline_metrics.dart';
import 'main_window_view_handles.dart';
import 'main_window_view_model.dart';
import 'main_window_view_model_factory.dart';

part 'main_window_action_binder.dart';
part 'main_window_composition.dart';

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
  AgentProtocolServer? _agentServer;
  bool _rendererOwnedFlutterSurfaceRequestQueued = false;
  bool _rendererOwnedFlutterSurfacePostFrameQueued = false;
  String? _lastRendererOwnedFlutterSurfaceRequestReason;
  String? _lastRendererOwnedFlutterSurfaceUiSignature;
  int _rendererOwnedFlutterSurfaceUiDirtyCount = 0;
  int _rendererOwnedFlutterSurfaceRequestCount = 0;
  int _rendererOwnedFlutterSurfaceSkippedClockStateCount = 0;
  DateTime? _lastRendererOwnedFlutterSurfaceBoostAt;
  static const Duration _rendererOwnedFlutterSurfaceBoostInterval = Duration(
    milliseconds: 100,
  );

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
    _maybeStartAgentServer();
    _maybeStartTestRunner(testScriptPath);
  }

  void _scheduleRendererOwnedViewportRectReport() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted()) return;
      _reportRendererOwnedViewportRect(reason: 'runner-layer-active-postframe');
    });
    WidgetsBinding.instance.scheduleFrame();
    Timer(const Duration(milliseconds: 50), () {
      if (!mounted() || !stateStore.value.nativeCompositorRunnerLayerActive) {
        return;
      }
      _reportRendererOwnedViewportRect(reason: 'runner-layer-active-deferred');
    });
  }

  void _reportRendererOwnedViewportRect({required String reason}) {
    final context = viewportKey.currentContext;
    if (context == null) {
      log.fine(
        '[WindowsResizePacing] renderer-owned viewportRect skipped '
        'reason=$reason missingContext=true',
      );
      return;
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      log.fine(
        '[WindowsResizePacing] renderer-owned viewportRect skipped '
        'reason=$reason hasRenderBox=${renderObject is RenderBox} '
        'hasSize=${renderObject is RenderBox && renderObject.hasSize}',
      );
      return;
    }
    final view = View.of(context);
    final devicePixelRatio = view.devicePixelRatio;
    final surfaceSize = view.physicalSize;
    final logicalSize = renderObject.size;
    if (devicePixelRatio <= 0 ||
        logicalSize.width <= 0 ||
        logicalSize.height <= 0 ||
        surfaceSize.width <= 0 ||
        surfaceSize.height <= 0) {
      log.fine(
        '[WindowsResizePacing] renderer-owned viewportRect skipped '
        'reason=$reason invalidSize=true',
      );
      return;
    }
    final globalOffset = renderObject.localToGlobal(Offset.zero);
    final left = (globalOffset.dx * devicePixelRatio).round();
    final top = (globalOffset.dy * devicePixelRatio).round();
    final width = (logicalSize.width * devicePixelRatio).round();
    final height = (logicalSize.height * devicePixelRatio).round();
    final surfaceWidth = surfaceSize.width.round();
    final surfaceHeight = surfaceSize.height.round();
    log.info(
      '[WindowsResizePacing] renderer-owned viewportRect '
      'reason=$reason physical=($left,$top ${width}x$height) '
      'surface=${surfaceWidth}x$surfaceHeight '
      'logicalOffset=(${globalOffset.dx.toStringAsFixed(1)},'
      '${globalOffset.dy.toStringAsFixed(1)}) '
      'logicalSize=${logicalSize.width.toStringAsFixed(1)}x'
      '${logicalSize.height.toStringAsFixed(1)} '
      'dpr=${devicePixelRatio.toStringAsFixed(3)}',
    );
    fireAndLog(
      'set renderer-owned viewport rect ($reason)',
      player.setRendererOwnedViewportRect(
        left: left,
        top: top,
        width: width,
        height: height,
        surfaceWidth: surfaceWidth,
        surfaceHeight: surfaceHeight,
      ),
    );
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
    final agentServer = _agentServer;
    _agentServer = null;
    if (agentServer != null) await agentServer.dispose();
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
    _queueRendererOwnedFlutterSurfaceRequestIfUiChanged();
  }

  void _queueRendererOwnedFlutterSurfaceRequestIfUiChanged() {
    if (!(Platform.isWindows || Platform.isMacOS) ||
        !_nativeCompositorActive ||
        !player.canAcceptCommands) {
      _lastRendererOwnedFlutterSurfaceUiSignature = null;
      return;
    }
    final signature = _rendererOwnedFlutterSurfaceUiSignature();
    if (_lastRendererOwnedFlutterSurfaceUiSignature == signature) {
      _rendererOwnedFlutterSurfaceSkippedClockStateCount += 1;
      return;
    }
    _lastRendererOwnedFlutterSurfaceUiSignature = signature;
    _rendererOwnedFlutterSurfaceUiDirtyCount += 1;
    _queueRendererOwnedFlutterSurfaceRequest(
      reason: _rendererOwnedFlutterSurfaceRequestReason(signature),
    );
  }

  String _rendererOwnedFlutterSurfaceUiSignature() {
    final state = stateStore.value;
    final trackOrder = trackManager.entries
        .map((entry) => '${entry.fileId}:${entry.slot}')
        .join(',');
    final quickMarks = state.quickMarks
        .map((mark) => '${mark.id}:${mark.fileId}')
        .join(',');
    final quickMarkDraft = state.quickMarkDraft;
    final thumbnailSignature = state.quickMarkThumbnails.entries
        .map((entry) => '${entry.key}:${entry.value.status.name}')
        .join(',');
    return [
      'tracks=$trackOrder',
      'sidebar=${state.marksSidebarVisible}',
      'sidebarWidth=${state.marksSidebarWidth.round()}',
      'mediaInfo=${state.mediaInfoVisible}',
      'profiler=${state.profilerVisible}',
      'settings=${state.settingsVisible}',
      'dragging=${state.dragging}',
      'analysisOverlay=${state.analysisOverlayControlsVisible}',
      'fullscreen=${state.fullScreen}',
      'fullscreenControls=${state.fullScreenControlsVisible}',
      'viewport=${state.viewportState.status.name}',
      'surface=${layoutCoordinator.viewportWidth}x'
          '${layoutCoordinator.viewportHeight}',
      'texture=${state.textureId}',
      'runnerLayer=${state.nativeCompositorRunnerLayerActive}',
      'quickMarks=$quickMarks',
      'quickMarkDraft=${quickMarkDraft == null ? 'none' : '${quickMarkDraft.id}:${quickMarkDraft.fileId}'}',
      'quickMarkSelected=${state.selectedQuickMarkId}',
      'quickMarkThumbs=$thumbnailSignature',
      'timelineControls=${state.timelineControlsWidth.round()}',
      'loop=${state.loopRangeEnabled}:${state.loopStartUs}:${state.loopEndUs}',
    ].join('|');
  }

  String _rendererOwnedFlutterSurfaceRequestReason(String signature) {
    return 'ui-surface-changed ${signature.hashCode} '
        'dirty=$_rendererOwnedFlutterSurfaceUiDirtyCount '
        'requests=$_rendererOwnedFlutterSurfaceRequestCount '
        'skippedClock=$_rendererOwnedFlutterSurfaceSkippedClockStateCount';
  }

  void _onRendererOwnedPresentationResizeCommitted(int width, int height) {
    _lastRendererOwnedFlutterSurfaceUiSignature = null;
    _rendererOwnedFlutterSurfaceUiDirtyCount += 1;
    _queueRendererOwnedFlutterSurfaceRequest(reason: 'resize ${width}x$height');
  }

  void _queueRendererOwnedFlutterSurfaceRequest({required String reason}) {
    if (_rendererOwnedFlutterSurfaceRequestQueued ||
        !(Platform.isWindows || Platform.isMacOS) ||
        !_nativeCompositorActive ||
        !player.canAcceptCommands) {
      if (!_nativeCompositorActive) {
        _lastRendererOwnedFlutterSurfaceRequestReason = null;
      }
      return;
    }
    if (_lastRendererOwnedFlutterSurfaceRequestReason == reason) {
      return;
    }
    log.fine('[RendererOwnedFlutterSurface] queue export: $reason');
    _rendererOwnedFlutterSurfaceRequestQueued = true;
    void request() {
      _rendererOwnedFlutterSurfaceRequestQueued = false;
      if (!_nativeCompositorActive || !player.canAcceptCommands) {
        if (!_nativeCompositorActive) {
          _lastRendererOwnedFlutterSurfaceRequestReason = null;
        }
        return;
      }
      _lastRendererOwnedFlutterSurfaceRequestReason = reason;
      _rendererOwnedFlutterSurfaceRequestCount += 1;
      fireAndLog(
        'request renderer-owned Flutter surface',
        player.requestRendererOwnedFlutterSurface(reason: reason),
      );
    }

    if (Platform.isMacOS) {
      if (_rendererOwnedFlutterSurfacePostFrameQueued) {
        WidgetsBinding.instance.ensureVisualUpdate();
        return;
      }
      _rendererOwnedFlutterSurfacePostFrameQueued = true;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        _rendererOwnedFlutterSurfacePostFrameQueued = false;
        request();
      });
      WidgetsBinding.instance.ensureVisualUpdate();
    } else {
      scheduleMicrotask(request);
    }
  }

  void _boostRendererOwnedFlutterSurfaceInteraction({required String reason}) {
    if (!(Platform.isWindows || Platform.isMacOS) ||
        !_nativeCompositorActive ||
        !player.canAcceptCommands) {
      return;
    }
    final now = DateTime.now();
    final last = _lastRendererOwnedFlutterSurfaceBoostAt;
    if (last != null &&
        now.difference(last) < _rendererOwnedFlutterSurfaceBoostInterval) {
      return;
    }
    _lastRendererOwnedFlutterSurfaceBoostAt = now;
    fireAndLogFine(
      'boost renderer-owned Flutter surface interaction',
      player.boostRendererOwnedFlutterSurfaceInteraction(reason: reason),
    );
  }

  Map<String, Object?> rendererOwnedFlutterSurfaceDartDiagnostics() => {
    'rendererOwnedFlutterSurfaceUiDirtyCount':
        _rendererOwnedFlutterSurfaceUiDirtyCount,
    'rendererOwnedFlutterSurfaceRequestCount':
        _rendererOwnedFlutterSurfaceRequestCount,
    'rendererOwnedFlutterSurfaceSkippedClockStateCount':
        _rendererOwnedFlutterSurfaceSkippedClockStateCount,
    'rendererOwnedFlutterSurfaceRequestQueued':
        _rendererOwnedFlutterSurfaceRequestQueued,
    'rendererOwnedFlutterSurfacePostFrameQueued':
        _rendererOwnedFlutterSurfacePostFrameQueued,
    'rendererOwnedFlutterSurfaceLastRequestReason':
        _lastRendererOwnedFlutterSurfaceRequestReason ?? '',
  };

  MainWindowViewModel get viewModel {
    final markView = quickMarkCoordinator.view;
    return MainWindowViewModelFactory.build(
      session: _session,
      layout: _layout,
      textureId: _textureId,
      nativeCompositorActive: _nativeCompositorActive,
      nativeCompositorRunnerLayerActive:
          _state.nativeCompositorRunnerLayerActive,
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

  void _requestAnalysisOverlayRedraw() {
    if (!mounted()) return;
    fireAndLog('refresh analysis overlay', _refreshAnalysisOverlay());
  }

  Future<void> _refreshAnalysisOverlay() async {
    await player.setNativeAnalysisOverlay(
      analysisGeneration.nativeOverlayStatePayload(),
    );
    layoutCoordinator.refreshRendererOwnedOverlay();
    await player.applyLayout(_layout);
  }

  void _onTrackManagerChanged() {
    stateStore.setLayout(_layout.copyWith(order: trackManager.order));
    layoutCoordinator.onTrackSetChanged();
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
  bool get _nativeCompositorActive => _state.nativeCompositorActive;
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
