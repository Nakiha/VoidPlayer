import 'dart:async';

import 'package:flutter/material.dart';

import '../../actions/action_registry.dart';
import '../../analysis/analysis_manager.dart';
import '../../analysis/analysis_toolbar_data_source.dart';
import '../../app_log.dart';
import '../../automation/test_runner.dart';
import '../../automation/ui_automation_bridge.dart';
import '../../config/app_config.dart';
import '../../config/app_settings_repository.dart';
import '../../marks/quick_mark.dart';
import '../../marks/quick_mark_persistence.dart';
import '../../marks/quick_mark_store.dart';
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
import '../../viewport/display_geometry.dart';
import '../../viewport/viewport_display_state.dart';
import '../../widgets/loop_range_bar.dart';
import 'main_window_actions.dart';
import 'main_window_analysis.dart';
import 'main_window_layout.dart';
import 'main_window_media.dart';
import 'main_window_media_lifecycle.dart';
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
  final AnalysisProcessHost analysisProcesses;
  final PlatformCapabilities platformCapabilities;
  final NativeFilePicker nativeFilePicker;
  final AnalysisGenerationService analysisGeneration;
  final AnalysisToolbarDataSource analysisToolbarDataSource;
  final AppSettingsRepository appSettings;
  final PlaybackPreferences playbackPreferences;
  final QuickMarkRepository quickMarkRepository;

  final NativePlayerController player = NativePlayerController();
  final TrackManager trackManager = TrackManager();
  final MainWindowStateStore stateStore = MainWindowStateStore();
  final PlaybackSession _session = const PlaybackSession.normal();
  ViewportSourceHit? _quickMarkDragStart;
  Offset? _quickMarkDragLatestPhysicalPosition;
  QuickMarkAnchor? _quickMarkDragAnchor;
  Future<QuickMarkAnchor>? _quickMarkDragAnchorFuture;
  int _quickMarkDragSerial = 0;
  int _nextQuickMarkId = 1;
  int _quickMarkPersistenceSerial = 0;
  String _quickMarkTrackSignature = '';
  Timer? _quickMarkSaveTimer;
  Future<void>? _quickMarkSaveInFlight;
  bool _quickMarkSavePending = false;
  List<QuickMarkMediaRef> _pendingQuickMarkSaveRefs = const [];
  List<QuickMark> _pendingQuickMarkSaveMarks = const [];
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
  Timer? _fullScreenControlsTimer;
  int _fullScreenSerial = 0;
  bool? _pendingFullScreen;
  bool _fullScreenUiResizePending = false;
  Future<void>? _shutdownFuture;
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
    _fullScreenControlsTimer?.cancel();
    _quickMarkSaveTimer?.cancel();
    actionCoordinator.dispose();
    playbackCoordinator.dispose();
    await _flushQuickMarkSave();
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

  MainWindowViewModel get viewModel {
    final markView = _quickMarkStore.view(
      context: _quickMarkFrameContext,
      selectedMarkId: _selectedQuickMarkId,
    );
    return MainWindowViewModel(
      fullFrameCaptureKey: fullFrameCaptureKey,
      session: MainWindowSessionVm.fromSession(_session),
      viewport: MainWindowViewportVm(
        viewMode: _layout.mode,
        viewModeEnabled: _textureId != null,
        textureId: _textureId,
        viewportState: _viewportState,
        layout: _layout,
        viewportKey: viewportKey,
        tracks: trackManager.entries
            .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry.info))
            .toList(),
        quickMarks: markView.visibleMarks,
        quickMarkDraft: _quickMarkDraft,
        selectedQuickMarkId: markView.visibleSelectedMarkId,
      ),
      marks: MainWindowMarksVm(
        allMarks: markView.allMarks,
        visibleMarks: markView.visibleMarks,
        visibleMarkIds: markView.visibleMarkIds,
        selectedMarkId: markView.selectedMarkId,
        tracksByFileId: {
          for (final entry in trackManager.entries) entry.fileId: entry.info,
        },
        currentPtsUs: _currentPtsUs,
      ),
      media: MainWindowMediaVm(
        analysisEnabled:
            platformCapabilities.externalAnalysisWindows &&
            trackManager.count > 0,
        analysisOverlayEnabled:
            platformCapabilities.analysisOverlays && trackManager.count > 0,
        nativePlaybackAvailable:
            platformCapabilities.nativePlayback ||
            platformCapabilities.localFilePlayback,
        localFilePlaybackAvailable: platformCapabilities.localFilePlayback,
        networkMediaAvailable: platformCapabilities.networkMediaPlayback,
        sshRemoteMediaAvailable: platformCapabilities.sshRemoteMediaPlayback,
        nativeFilePickerAvailable: platformCapabilities.nativeFilePicker,
        tracks: trackManager.entries,
        syncOffsets: _syncOffsets,
        audibleTrackFileId: _audibleTrackFileId,
        performanceAlertPolicy: _performanceAlertPolicy,
        analysisDataSource: analysisToolbarDataSource,
        analysisOverlayButtonKey: analysisOverlayButtonKey,
      ),
      playback: MainWindowPlaybackVm(
        timelineSliderKey: timelineSliderKey,
        controlsBarKey: controlsBarKey,
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
        mediaInfoVisible: _mediaInfoVisible,
        profilerVisible: _profilerVisible,
        settingsVisible: _settingsVisible,
        marksSidebarVisible: _marksSidebarVisible,
        marksSidebarWidth: _marksSidebarWidth,
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
          if (!_capabilities.canChangeViewMode) return;
          layoutCoordinator.setLayoutMode(mode);
        },
        onOpenFile: () {
          if (!_capabilities.canOpenLocalMedia || !_capabilities.canAddTrack) {
            return Future<void>.value();
          }
          return mediaCoordinator.openFile();
        },
        onOpenNetworkMedia: (url) {
          if (!_capabilities.canOpenNetworkMedia ||
              !_capabilities.canAddTrack) {
            return Future<void>.value();
          }
          return mediaCoordinator.addNetworkMedia(url);
        },
        onOpenSshRemoteMedia: (remotePath) {
          if (!_capabilities.canOpenSshMedia || !_capabilities.canAddTrack) {
            return Future<void>.value();
          }
          return mediaCoordinator.addSshRemoteMedia(remotePath);
        },
        onMediaInfo: () {
          if (!_capabilities.canOpenMediaInfo || trackManager.isEmpty) return;
          stateStore.setMediaInfoVisible(!_mediaInfoVisible);
        },
        onAnalysis: () {
          if (!_capabilities.canRunAnalysis) return Future<void>.value();
          return analysisCoordinator.triggerAnalysis();
        },
        onAnalysisOverlayPanelToggle: () {
          if (!_capabilities.canShowAnalysisOverlay) {
            return Future<void>.value();
          }
          return analysisCoordinator.toggleOverlayPanel();
        },
        onProfiler: () {
          if (!_capabilities.canOpenProfiler) return;
          stateStore.setProfilerVisible(!_profilerVisible);
        },
        onSettings: () => stateStore.setSettingsVisible(!_settingsVisible),
        onMarksSidebarToggle: _toggleMarksSidebar,
      ),
      viewport: MainWindowViewportActions(
        onPan: (delta) {
          if (!_capabilities.canPanViewport) return;
          layoutCoordinator.onPan(delta);
        },
        onSplit: layoutCoordinator.onSplit,
        onZoom: (factor, localPos) {
          if (!_capabilities.canZoomViewport) return;
          layoutCoordinator.onZoom(factor, localPos);
        },
        onPointerButton: layoutCoordinator.onPointerButton,
        onResize: (width, height, devicePixelRatio) =>
            layoutCoordinator.onViewportResize(
              width,
              height,
              devicePixelRatio,
              immediate: _fullScreenUiResizePending,
            ),
        onQuickMarkStart: _startQuickMarkDrag,
        onQuickMarkUpdate: _updateQuickMarkDrag,
        onQuickMarkEnd: _finishQuickMarkDrag,
        onQuickMarkCancel: _cancelQuickMarkDrag,
        onQuickMarkSelect: _selectQuickMark,
        onQuickMarkChanged: _updateQuickMark,
        onQuickMarkDeleted: _deleteQuickMark,
        onQuickMarkFocus: _focusQuickMark,
      ),
      marks: MainWindowMarksActions(
        onJumpToMark: _jumpToQuickMark,
        onSelectVisibleMark: _selectQuickMark,
        onMarkChanged: _updateQuickMark,
        onMarkDeleted: _deleteQuickMark,
        onFocusVisibleMark: _focusQuickMark,
      ),
      mediaTimeline: MainWindowMediaTimelineActions(
        onMediaSwapped: (slotIndex, targetTrackIndex) {
          if (!_capabilities.canReorderTrack) return;
          mediaCoordinator.onMediaSwapped(slotIndex, targetTrackIndex);
        },
        onRemoveTrack: (fileId) {
          if (!_capabilities.canRemoveTrack) return;
          fireAndLog('remove track', _removeTrack(fileId));
        },
        onZoomChanged: (value) {
          if (!_capabilities.canZoomViewport) return;
          layoutCoordinator.onZoomComboChanged(value);
        },
        onToggleFullScreen: _toggleFullScreen,
        onTogglePlay: playbackCoordinator.togglePlayPause,
        onStepForward: () =>
            fireAndLog('step forward', playbackCoordinator.stepForward()),
        onStepBackward: () =>
            fireAndLog('step backward', playbackCoordinator.stepBackward()),
        onSeek: (ptsUs) {
          if (!_capabilities.canSeek) return;
          playbackCoordinator.seekTo(ptsUs);
        },
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
        onReorder: (oldIndex, newIndex) {
          if (!_capabilities.canReorderTrack) return;
          trackManager.moveTrack(oldIndex, newIndex);
        },
        onOffsetChanged: (slot, offsetMs) {
          if (!_capabilities.canAdjustTrackOffset) return;
          mediaCoordinator.onOffsetChanged(slot, offsetMs);
        },
        onToggleTrackAudio: (fileId) {
          if (!_capabilities.canToggleTrackAudio) return;
          _toggleTrackAudio(fileId);
        },
        onControlsWidthChanged: stateStore.setTimelineControlsWidth,
      ),
      analysisOverlay: MainWindowAnalysisOverlayActions(
        onTypeChanged: (type) {
          final config = analysisGeneration.overlayConfig.withTypeDefaults(
            type,
          );
          analysisCoordinator.updateOverlayConfig(config);
        },
        onOpacityChanged: (opacity) {
          final config = analysisGeneration.overlayConfig.copyWith(
            opacity: opacity,
          );
          analysisCoordinator.updateOverlayConfig(config);
        },
        onActivate: analysisCoordinator.activateOverlayPanelTracks,
        onClose: analysisCoordinator.deactivateOverlay,
      ),
      overlays: MainWindowOverlayActions(
        onCloseMediaInfo: () => stateStore.setMediaInfoVisible(false),
        onCloseProfiler: () => stateStore.setProfilerVisible(false),
        onCloseSettings: () => stateStore.setSettingsVisible(false),
        onCloseMarksSidebar: () => _setMarksSidebarVisible(false),
        onMarksSidebarWidthChanged: _setMarksSidebarWidth,
        onViewportPixelSizeModeChanged: _setViewportPixelSizeMode,
        onPerformanceAlertPolicyChanged: _setPerformanceAlertPolicy,
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

  void _setMarksSidebarWidth(double width) {
    final next = width
        .clamp(kMinMarksSidebarWidth, kMaxMarksSidebarWidth)
        .toDouble();
    final delta = next - _marksSidebarWidth;
    if (delta == 0) return;
    stateStore.setMarksSidebarWidth(next);
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
    stateStore.setLayout(
      _layout.copyWith(
        pixelSizeMode: playbackPreferences.viewportPixelSizeMode.layoutValue,
      ),
    );
    stateStore.setPerformanceAlertPolicy(
      playbackPreferences.performanceAlertPolicy,
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
      playbackPreferences: playbackPreferences,
      nativeFilePicker: nativeFilePicker,
      appSettings: appSettings,
      mounted: mounted,
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
      toggleFullScreen: _toggleFullScreen,
      exitFullScreen: _exitFullScreen,
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
    _reconcileQuickMarkPersistence();
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
  List<QuickMark> get _quickMarks => _state.quickMarks;
  QuickMarkStore get _quickMarkStore =>
      QuickMarkStore(marks: _quickMarks, nextId: _nextQuickMarkId);
  QuickMarkFrameContext get _quickMarkFrameContext => QuickMarkFrameContext(
    currentPtsUs: _currentPtsUs,
    presentedFrameAnchors: _presentedFrameAnchors,
  );
  QuickMark? get _quickMarkDraft => _state.quickMarkDraft;
  int? get _selectedQuickMarkId => _state.selectedQuickMarkId;
  Map<int, QuickMarkAnchor> get _presentedFrameAnchors =>
      _state.presentedFrameAnchors;
  Map<int, int> get _syncOffsets => _state.syncOffsets;
  double get _timelineControlsWidth => _state.timelineControlsWidth;
  bool get _loopRangeEnabled => _state.loopRangeEnabled;
  bool get _dragging => _state.dragging;
  bool get _mediaInfoVisible => _state.mediaInfoVisible;
  bool get _profilerVisible => _state.profilerVisible;
  bool get _settingsVisible => _state.settingsVisible;
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

  List<QuickMarkMediaRef> _quickMarkMediaRefs() => [
    for (final entry in trackManager.entries)
      QuickMarkMediaRef(fileId: entry.fileId, path: entry.path),
  ];

  String _quickMarkMediaSignature(List<QuickMarkMediaRef> refs) {
    return refs.map((ref) => '${ref.fileId}:${ref.mediaId}').join('|');
  }

  void _reconcileQuickMarkPersistence() {
    final refs = _quickMarkMediaRefs();
    final signature = _quickMarkMediaSignature(refs);
    if (signature == _quickMarkTrackSignature) return;
    _quickMarkTrackSignature = signature;
    final serial = ++_quickMarkPersistenceSerial;
    unawaited(_loadQuickMarksForMediaRefs(refs, signature, serial));
  }

  Future<void> _loadQuickMarksForMediaRefs(
    List<QuickMarkMediaRef> refs,
    String signature,
    int serial,
  ) async {
    await _flushQuickMarkSave();
    if (serial != _quickMarkPersistenceSerial ||
        signature != _quickMarkTrackSignature) {
      return;
    }
    if (refs.isEmpty) return;
    try {
      final loaded = await quickMarkRepository.loadForMediaRefs(refs);
      if (serial != _quickMarkPersistenceSerial ||
          signature != _quickMarkTrackSignature) {
        return;
      }
      final loadedFileIds = loaded.map((mark) => mark.fileId).toSet();
      final next = [
        for (final mark in _quickMarks)
          if (!loadedFileIds.contains(mark.fileId)) mark,
        ...loaded,
      ];
      _applyQuickMarkStore(
        QuickMarkStore(marks: next, nextId: _nextQuickMarkId),
        persist: false,
      );
    } catch (e, stack) {
      log.warning('[QuickMark] load failed', e, stack);
    }
  }

  void _scheduleQuickMarkSave() {
    _pendingQuickMarkSaveRefs = _quickMarkMediaRefs();
    _pendingQuickMarkSaveMarks = List<QuickMark>.unmodifiable(_quickMarks);
    _quickMarkSavePending = true;
    _quickMarkSaveTimer?.cancel();
    _quickMarkSaveTimer = Timer(
      const Duration(milliseconds: 300),
      () => unawaited(_savePendingQuickMarksNow()),
    );
  }

  Future<void> _flushQuickMarkSave() async {
    _quickMarkSaveTimer?.cancel();
    _quickMarkSaveTimer = null;
    await _savePendingQuickMarksNow();
  }

  Future<void> _savePendingQuickMarksNow() async {
    final previous = _quickMarkSaveInFlight;
    if (previous != null) {
      try {
        await previous;
      } catch (_) {
        // The original save call logs failures; later saves should continue.
      }
    }
    if (!_quickMarkSavePending) return;
    final refs = _pendingQuickMarkSaveRefs;
    final marks = _pendingQuickMarkSaveMarks;
    _quickMarkSavePending = false;
    _pendingQuickMarkSaveRefs = const [];
    _pendingQuickMarkSaveMarks = const [];
    if (refs.isEmpty) return;
    final save = quickMarkRepository.saveForMediaRefs(refs, marks);
    _quickMarkSaveInFlight = save;
    try {
      await save;
    } catch (e, stack) {
      log.warning('[QuickMark] save failed', e, stack);
    } finally {
      if (identical(_quickMarkSaveInFlight, save)) {
        _quickMarkSaveInFlight = null;
      }
    }
  }

  ViewportLayoutProjection? _quickMarkProjection() {
    final viewportWidth = layoutCoordinator.viewportWidth;
    final viewportHeight = layoutCoordinator.viewportHeight;
    if (viewportWidth <= 0 || viewportHeight <= 0 || trackManager.isEmpty) {
      return null;
    }
    return computeViewportLayoutProjection(
      viewportWidth: viewportWidth,
      viewportHeight: viewportHeight,
      layout: _layout,
      tracks: trackManager.entries
          .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry.info))
          .toList(),
    );
  }

  bool _isQuickMarkVisible(QuickMark mark) {
    return _quickMarkStore.isVisible(mark, _quickMarkFrameContext);
  }

  Future<QuickMarkAnchor> _quickMarkAnchorForFileId(int fileId) async {
    PresentedFrameTiming? timing;
    try {
      timing = await player.currentPresentedFrame(fileId);
    } catch (_) {
      timing = null;
    }
    return QuickMarkAnchor.fromPresentedFrame(
      fileId: fileId,
      timing: timing,
      fallbackPtsUs: _currentPtsUs,
    );
  }

  QuickMark _quickMarkDraftForDrag({
    required ViewportSourceHit start,
    required Offset end,
    required QuickMarkAnchor anchor,
  }) {
    return QuickMark(
      id: 0,
      anchor: anchor,
      sourceRect: Rect.fromPoints(start.sourceUv, end),
      sourceStart: start.sourceUv,
      sourceEnd: end,
    );
  }

  void _updateQuickMarkDraftForDrag(Offset physicalPosition) {
    final start = _quickMarkDragStart;
    final anchor = _quickMarkDragAnchor;
    if (start == null || anchor == null) return;
    final projection = _quickMarkProjection();
    final end = projection?.sourceUvForTrackPhysical(
      start.fileId,
      physicalPosition,
      clipToVisibleRegion: true,
    );
    if (end == null) return;
    stateStore.setQuickMarkDraft(
      _quickMarkDraftForDrag(start: start, end: end, anchor: anchor),
    );
  }

  void _startQuickMarkDrag(Offset physicalPosition) {
    if (_textureId == null || trackManager.isEmpty) return;
    final hit = _quickMarkProjection()?.hitTestPhysical(physicalPosition);
    if (hit == null) return;
    stateStore.setSelectedQuickMarkId(null);
    _quickMarkDragStart = hit;
    _quickMarkDragLatestPhysicalPosition = physicalPosition;
    final serial = ++_quickMarkDragSerial;
    final initialAnchor =
        _presentedFrameAnchors[hit.fileId] ??
        QuickMarkAnchor.fromPresentedFrame(
          fileId: hit.fileId,
          timing: null,
          fallbackPtsUs: _currentPtsUs,
        );
    _quickMarkDragAnchor = initialAnchor;
    _quickMarkDragAnchorFuture = _isPlaying
        ? null
        : _quickMarkAnchorForFileId(hit.fileId);
    stateStore.setQuickMarkDraft(
      _quickMarkDraftForDrag(
        start: hit,
        end: hit.sourceUv,
        anchor: initialAnchor,
      ),
    );
    final anchorFuture = _quickMarkDragAnchorFuture;
    if (anchorFuture != null) {
      unawaited(
        anchorFuture.then((anchor) {
          if (_quickMarkDragSerial != serial || _quickMarkDragStart != hit) {
            return;
          }
          _quickMarkDragAnchor = anchor;
          final latest = _quickMarkDragLatestPhysicalPosition;
          if (latest != null) {
            _updateQuickMarkDraftForDrag(latest);
          }
        }),
      );
    }
  }

  void _updateQuickMarkDrag(Offset physicalPosition) {
    _quickMarkDragLatestPhysicalPosition = physicalPosition;
    _updateQuickMarkDraftForDrag(physicalPosition);
  }

  void _finishQuickMarkDrag() async {
    final serial = _quickMarkDragSerial;
    final anchorFuture = _quickMarkDragAnchorFuture;
    if (anchorFuture != null) {
      final anchor = await anchorFuture;
      if (_quickMarkDragSerial != serial) return;
      if (_quickMarkDragSerial == serial && _quickMarkDragStart != null) {
        _quickMarkDragAnchor = anchor;
        final latest = _quickMarkDragLatestPhysicalPosition;
        if (latest != null) {
          _updateQuickMarkDraftForDrag(latest);
        }
      }
    }
    final draft = _quickMarkDraft;
    _quickMarkDragStart = null;
    _quickMarkDragLatestPhysicalPosition = null;
    _quickMarkDragAnchor = null;
    _quickMarkDragAnchorFuture = null;
    _quickMarkDragSerial++;
    stateStore.setQuickMarkDraft(null);
    if (draft == null ||
        draft.sourceRect.width < 0.002 ||
        draft.sourceRect.height < 0.002) {
      return;
    }
    _applyQuickMarkStore(_quickMarkStore.add(draft));
  }

  void _cancelQuickMarkDrag() {
    _quickMarkDragStart = null;
    _quickMarkDragLatestPhysicalPosition = null;
    _quickMarkDragAnchor = null;
    _quickMarkDragAnchorFuture = null;
    _quickMarkDragSerial++;
    stateStore.setQuickMarkDraft(null);
  }

  void _selectQuickMark(int? id) {
    if (id != null) {
      final mark = _quickMarkStore.markById(id);
      if (mark == null || !_isQuickMarkVisible(mark)) return;
    }
    stateStore.setSelectedQuickMarkId(id);
  }

  void _updateQuickMark(QuickMark updated) {
    _applyQuickMarkStore(_quickMarkStore.update(updated));
  }

  void _deleteQuickMark(int id) {
    _applyQuickMarkStore(_quickMarkStore.delete(id));
    if (_selectedQuickMarkId == id) {
      stateStore.setSelectedQuickMarkId(null);
    }
  }

  void _deleteQuickMarksForFileId(int fileId) {
    final nextStore = _quickMarkStore.deleteForFileId(fileId);
    _applyQuickMarkStore(nextStore);
    if (_selectedQuickMarkId != null &&
        !nextStore.contains(_selectedQuickMarkId!)) {
      stateStore.setSelectedQuickMarkId(null);
    }
  }

  void _focusQuickMark(int id) {
    final mark = _quickMarkStore.markById(id);
    if (mark == null || !_isQuickMarkVisible(mark)) return;
    stateStore.setSelectedQuickMarkId(id);
    layoutCoordinator.focusQuickMark(mark);
  }

  void _jumpToQuickMark(int id) {
    final mark = _quickMarkStore.markById(id);
    if (mark == null) return;
    stateStore.setSelectedQuickMarkId(id);
    playbackCoordinator.seekTo(mark.anchor.ptsUs);
  }

  void _applyQuickMarkStore(QuickMarkStore store, {bool persist = true}) {
    _nextQuickMarkId = store.nextId;
    stateStore.setQuickMarks(store.marks);
    if (persist) _scheduleQuickMarkSave();
  }

  Future<void> _removeTrack(int fileId) async {
    await mediaCoordinator.removeTrack(fileId);
    if (trackManager.entries.any((entry) => entry.fileId == fileId)) return;
    _deleteQuickMarksForFileId(fileId);
  }
}
