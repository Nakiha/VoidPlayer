part of 'main_window_controller.dart';

extension MainWindowViewActionBinding on MainWindowController {
  MainWindowViewActions _createViewActions() {
    return MainWindowViewActions(
      drop: MainWindowDropActions(
        filesDropped: (paths) {
          stateStore.setDragging(false);
          _fireUserAction(
            'load dropped media',
            () => mediaCoordinator.loadMediaPaths(paths),
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
          log.fine(
            '[WindowsCompositorDebug] toolbar viewMode click mode=$mode '
            'canChange=${_capabilities.canChangeViewMode}',
          );
          if (!_capabilities.canChangeViewMode) return;
          layoutCoordinator.setLayoutMode(mode);
        },
        onOpenFile: () {
          log.fine(
            '[WindowsCompositorDebug] toolbar openFile click '
            'canOpen=${_capabilities.canOpenLocalMedia} '
            'canAdd=${_capabilities.canAddTrack} '
            'tracks=${trackManager.count}',
          );
          if (!_capabilities.canOpenLocalMedia || !_capabilities.canAddTrack) {
            return Future<void>.value();
          }
          return _runUserAction('open file', mediaCoordinator.openFile);
        },
        onOpenNetworkMedia: (url) {
          log.fine(
            '[WindowsCompositorDebug] toolbar openNetwork click '
            'canOpen=${_capabilities.canOpenNetworkMedia} '
            'canAdd=${_capabilities.canAddTrack} '
            'tracks=${trackManager.count}',
          );
          if (!_capabilities.canOpenNetworkMedia ||
              !_capabilities.canAddTrack) {
            return Future<void>.value();
          }
          return _runUserAction(
            'open network media',
            () => mediaCoordinator.addNetworkMedia(url),
          );
        },
        onOpenSshRemoteMedia: (remotePath) {
          if (!_capabilities.canOpenSshMedia || !_capabilities.canAddTrack) {
            return Future<void>.value();
          }
          return _runUserAction(
            'open SSH remote media',
            () => mediaCoordinator.addSshRemoteMedia(remotePath),
          );
        },
        onMediaInfo: () {
          log.fine(
            '[WindowsCompositorDebug] toolbar mediaInfo click '
            'canOpen=${_capabilities.canOpenMediaInfo} '
            'tracks=${trackManager.count} visible=$_mediaInfoVisible',
          );
          if (!_capabilities.canOpenMediaInfo || trackManager.isEmpty) return;
          stateStore.setMediaInfoVisible(!_mediaInfoVisible);
        },
        onAnalysis: () {
          log.fine(
            '[WindowsCompositorDebug] toolbar analysis click '
            'canRun=${_capabilities.canRunAnalysis} '
            'tracks=${trackManager.count}',
          );
          if (!_capabilities.canRunAnalysis) return Future<void>.value();
          return _runUserAction(
            'run analysis',
            analysisCoordinator.triggerAnalysis,
          );
        },
        onAnalysisOverlayPanelToggle: () {
          if (!_capabilities.canShowAnalysisOverlay) {
            return Future<void>.value();
          }
          layoutCoordinator.setAnalysisOverlayControlsVisible(
            !_analysisOverlayControlsVisible,
          );
          return Future<void>.value();
        },
        onProfiler: () {
          log.fine(
            '[WindowsCompositorDebug] toolbar profiler click '
            'canOpen=${_capabilities.canOpenProfiler} '
            'visible=$_profilerVisible',
          );
          if (!_capabilities.canOpenProfiler) return;
          stateStore.setProfilerVisible(!_profilerVisible);
        },
        onSettings: () {
          log.fine(
            '[WindowsCompositorDebug] toolbar settings click '
            'visible=$_settingsVisible',
          );
          stateStore.setSettingsVisible(!_settingsVisible);
        },
        onMarksSidebarToggle: () {
          log.fine(
            '[WindowsCompositorDebug] toolbar marksSidebar click '
            'visible=$_marksSidebarVisible '
            'width=${_state.marksSidebarWidth}',
          );
          layoutCoordinator.toggleMarksSidebar();
        },
      ),
      viewport: MainWindowViewportActions(
        onPan: (delta) {
          if (!_capabilities.canPanViewport) return;
          _boostRendererOwnedFlutterSurfaceInteraction(reason: 'viewport-pan');
          layoutCoordinator.onPan(delta);
        },
        onSplit: (position) {
          _boostRendererOwnedFlutterSurfaceInteraction(
            reason: 'viewport-split',
          );
          layoutCoordinator.onSplit(position);
        },
        onZoom: (factor, localPos) {
          if (!_capabilities.canZoomViewport) return;
          _boostRendererOwnedFlutterSurfaceInteraction(reason: 'viewport-zoom');
          layoutCoordinator.onZoom(factor, localPos);
        },
        onPointerButton: layoutCoordinator.onPointerButton,
        onResize: (width, height, devicePixelRatio) =>
            layoutCoordinator.onViewportResize(
              width,
              height,
              devicePixelRatio,
              immediate: fullScreenCoordinator.uiResizePending,
            ),
        onNativeCompositorViewportRect:
            (left, top, width, height, surfaceWidth, surfaceHeight) {
              fireAndLog(
                'set renderer-owned viewport rect',
                player.setRendererOwnedViewportRect(
                  left: left,
                  top: top,
                  width: width,
                  height: height,
                  surfaceWidth: surfaceWidth,
                  surfaceHeight: surfaceHeight,
                ),
              );
            },
        onQuickMarkStart: (position) {
          _boostRendererOwnedFlutterSurfaceInteraction(
            reason: 'quick-mark-start',
          );
          quickMarkCoordinator.startDrag(position);
        },
        onQuickMarkUpdate: (position) {
          _boostRendererOwnedFlutterSurfaceInteraction(
            reason: 'quick-mark-drag',
          );
          quickMarkCoordinator.updateDrag(position);
        },
        onQuickMarkInteraction: () {
          _boostRendererOwnedFlutterSurfaceInteraction(
            reason: 'quick-mark-overlay',
          );
        },
        onQuickMarkEnd: quickMarkCoordinator.finishDrag,
        onQuickMarkCancel: quickMarkCoordinator.cancelDrag,
        onQuickMarkSelect: quickMarkCoordinator.select,
        onQuickMarkChanged: (mark) {
          _boostRendererOwnedFlutterSurfaceInteraction(
            reason: 'quick-mark-change',
          );
          quickMarkCoordinator.update(mark);
        },
        onQuickMarkDeleted: quickMarkCoordinator.delete,
        onQuickMarkFocus: quickMarkCoordinator.focus,
      ),
      marks: MainWindowMarksActions(
        onJumpToMark: quickMarkCoordinator.jumpTo,
        onSelectVisibleMark: quickMarkCoordinator.select,
        onMarkChanged: quickMarkCoordinator.update,
        onMarkDeleted: quickMarkCoordinator.delete,
        onFocusVisibleMark: quickMarkCoordinator.focus,
      ),
      mediaTimeline: MainWindowMediaTimelineActions(
        onMediaSwapped: (slotIndex, targetTrackIndex) {
          if (!_capabilities.canReorderTrack) return;
          mediaCoordinator.onMediaSwapped(slotIndex, targetTrackIndex);
        },
        onRemoveTrack: (fileId) {
          if (!_capabilities.canRemoveTrack) return Future<void>.value();
          return _runUserAction('remove track', () => _removeTrack(fileId));
        },
        onZoomChanged: (value) {
          if (!_capabilities.canZoomViewport) return;
          layoutCoordinator.onZoomComboChanged(value);
        },
        onToggleFullScreen: fullScreenCoordinator.toggle,
        onTogglePlay: () => _runUserAction(
          'toggle playback',
          playbackCoordinator.togglePlayPause,
        ),
        onStepForward: () =>
            _runUserAction('step forward', playbackCoordinator.stepForward),
        onStepBackward: () =>
            _runUserAction('step backward', playbackCoordinator.stepBackward),
        onSeek: (ptsUs) {
          if (!_capabilities.canSeek) return;
          _boostRendererOwnedFlutterSurfaceInteraction(reason: 'timeline-seek');
          playbackCoordinator.seekTo(ptsUs);
        },
        onSliderHover: (hoverUs, hovering) {
          if (hovering) {
            _boostRendererOwnedFlutterSurfaceInteraction(
              reason: 'timeline-hover',
            );
          }
          playbackCoordinator.onSliderHover(hoverUs, hovering);
        },
        onLoopRangeEnabledChanged: (enabled) {
          return _runUserAction(
            'set loop range enabled',
            () => playbackCoordinator.setLoopRangeEnabled(enabled),
          );
        },
        onLoopRangeChanged: (startUs, endUs) {
          _boostRendererOwnedFlutterSurfaceInteraction(
            reason: 'loop-range-drag',
          );
          playbackCoordinator.previewLoopRange(startUs, endUs);
        },
        onLoopRangeChangeEnd: (handle) {
          if (!_loopRangeEnabled) return Future<void>.value();
          return _runUserAction(
            'finish loop range change',
            () => playbackCoordinator.commitLoopRange(
              seekToStart: handle == LoopRangeHandle.start,
            ),
          );
        },
        onReorder: (oldIndex, newIndex) {
          if (!_capabilities.canReorderTrack) return;
          trackManager.moveTrack(oldIndex, newIndex);
        },
        onOffsetChanged: (fileId, offsetMs) {
          if (!_capabilities.canAdjustTrackOffset) return Future<void>.value();
          return _runUserAction(
            'adjust track offset',
            () => mediaCoordinator.onOffsetChanged(fileId, offsetMs),
          );
        },
        onToggleTrackAudio: (fileId) {
          if (!_capabilities.canToggleTrackAudio) return;
          playbackCoordinator.toggleTrackAudio(fileId);
        },
        onControlsWidthChanged: (width) {
          _boostRendererOwnedFlutterSurfaceInteraction(
            reason: 'timeline-controls-resize',
          );
          stateStore.setTimelineControlsWidth(width);
        },
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
        onActivate: () => _runUserAction(
          'activate analysis overlay',
          analysisCoordinator.activateOverlayPanelTracks,
        ),
        onClose: analysisCoordinator.deactivateOverlay,
      ),
      overlays: MainWindowOverlayActions(
        onCloseMediaInfo: () => stateStore.setMediaInfoVisible(false),
        onCloseProfiler: () => stateStore.setProfilerVisible(false),
        onCloseSettings: () => stateStore.setSettingsVisible(false),
        onCloseMarksSidebar: () =>
            layoutCoordinator.setMarksSidebarVisible(false),
        onMarksSidebarWidthChanged: layoutCoordinator.setMarksSidebarWidth,
        onViewportPixelSizeModeChanged: (mode) =>
            layoutCoordinator.setPixelSizeMode(mode.layoutValue),
        onPerformanceAlertPolicyChanged: stateStore.setPerformanceAlertPolicy,
        onFullScreenPointerActivity:
            fullScreenCoordinator.showControlsTemporarily,
        onFullScreenControlsHoverChanged:
            fullScreenCoordinator.setControlsHovering,
      ),
    );
  }
}
