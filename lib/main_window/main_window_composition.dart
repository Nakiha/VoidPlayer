part of 'main_window_controller.dart';

/// Composition root for the main window: wires coordinators together and
/// bootstraps the optional UI automation runner. Keep construction-order and
/// dependency decisions here so `MainWindowController` stays lifecycle-only.
extension MainWindowComposition on MainWindowController {
  void _initCoordinators() {
    layoutCoordinator = MainWindowLayoutCoordinator(
      vsync: vsync,
      controller: player,
      stateStore: stateStore,
      trackManager: trackManager,
      mounted: mounted,
      onNativeResizeCommitted: _onNativeCompositorResizeCommitted,
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
      onPlaybackTransition: ({required playing}) =>
          layoutCoordinator.onPlaybackStateChanged(playing: playing),
      onNativeCompositorAvailabilityChanged: ({required active}) =>
          layoutCoordinator.onNativeCompositorAvailabilityChanged(
            active: active,
          ),
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
      onMediaLoadRejected: onUserActionFailed == null
          ? null
          : (message) => onUserActionFailed!('Add media', message),
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

  void _maybeStartAgentServer() {
    final connectionFilePath = startupOptions.agentConnectionFile;
    if (connectionFilePath == null) return;
    final server = AgentProtocolServer(
      handler: MainWindowAgentHandler(
        stateStore: stateStore,
        trackManager: trackManager,
        mediaCoordinator: mediaCoordinator,
        playbackCoordinator: playbackCoordinator,
        quickMarkCoordinator: quickMarkCoordinator,
      ),
    );
    _agentServer = server;
    fireAndLog(
      'start agent protocol server',
      server.start(connectionFilePath: connectionFilePath),
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
          testHarness: testHarness,
          effectiveDurationUs: () => timelineMetrics.effectiveDurationUs,
          timelinePtsUs: () => stateStore.value.currentPtsUs,
          toggleAnalysisOverlayForSlot:
              analysisCoordinator.toggleOverlayForSlot,
          toggleAnalysisOverlayPanel: analysisCoordinator.toggleOverlayPanel,
          toggleMarksSidebar: layoutCoordinator.toggleMarksSidebar,
          generateAnalysisCacheForSlot:
              analysisCoordinator.ensureGeneratedForSlot,
          setMediaSourceIdForSlot: quickMarkCoordinator.declareSourceIdForSlot,
          exportMarksToFile: quickMarkCoordinator.exportMarksToFile,
          addQuickMark: (action) => quickMarkCoordinator.addAgentMark(
            slotIndex: action.slotIndex,
            sourceRect: Rect.fromLTWH(
              action.left,
              action.top,
              action.width,
              action.height,
            ),
            defectType: action.defectType,
            severity: action.severity,
          ),
          clearMarks: quickMarkCoordinator.clearAllMarks,
          quickMarkCount: () => quickMarkCoordinator.markCount,
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
          dartViewportDiagnostics: () =>
              ViewportInteractionDiagnostics.instance.snapshot(),
          actionRegistry: actionRegistry,
        ),
      ).run();
    });
  }
}
