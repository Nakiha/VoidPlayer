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
          if (!_capabilities.canChangeViewMode) return;
          layoutCoordinator.setLayoutMode(mode);
        },
        onOpenFile: () {
          if (!_capabilities.canOpenLocalMedia || !_capabilities.canAddTrack) {
            return Future<void>.value();
          }
          return _runUserAction('open file', mediaCoordinator.openFile);
        },
        onOpenNetworkMedia: (url) {
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
          if (!_capabilities.canOpenMediaInfo || trackManager.isEmpty) return;
          stateStore.setMediaInfoVisible(!_mediaInfoVisible);
        },
        onAnalysis: () {
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
          _setAnalysisOverlayControlsVisible(!_analysisOverlayControlsVisible);
          return Future<void>.value();
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
              immediate: fullScreenCoordinator.uiResizePending,
            ),
        onQuickMarkStart: quickMarkCoordinator.startDrag,
        onQuickMarkUpdate: quickMarkCoordinator.updateDrag,
        onQuickMarkEnd: quickMarkCoordinator.finishDrag,
        onQuickMarkCancel: quickMarkCoordinator.cancelDrag,
        onQuickMarkSelect: quickMarkCoordinator.select,
        onQuickMarkChanged: quickMarkCoordinator.update,
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
          playbackCoordinator.seekTo(ptsUs);
        },
        onSliderHover: playbackCoordinator.onSliderHover,
        onLoopRangeEnabledChanged: (enabled) {
          return _runUserAction(
            'set loop range enabled',
            () => playbackCoordinator.setLoopRangeEnabled(enabled),
          );
        },
        onLoopRangeChanged: playbackCoordinator.previewLoopRange,
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
        onOffsetChanged: (slot, offsetMs) {
          if (!_capabilities.canAdjustTrackOffset) return Future<void>.value();
          return _runUserAction(
            'adjust track offset',
            () => mediaCoordinator.onOffsetChanged(slot, offsetMs),
          );
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
        onCloseMarksSidebar: () => _setMarksSidebarVisible(false),
        onMarksSidebarWidthChanged: _setMarksSidebarWidth,
        onViewportPixelSizeModeChanged: _setViewportPixelSizeMode,
        onPerformanceAlertPolicyChanged: _setPerformanceAlertPolicy,
        onFullScreenPointerActivity:
            fullScreenCoordinator.showControlsTemporarily,
        onFullScreenControlsHoverChanged:
            fullScreenCoordinator.setControlsHovering,
      ),
    );
  }
}
