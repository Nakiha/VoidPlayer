import 'dart:async';

import '../../actions/action_registry.dart';
import '../../actions/player_action.dart';
import '../../session/playback_session.dart';
import '../../video_renderer_controller.dart';
import 'main_window_analysis.dart';
import 'main_window_layout.dart';
import 'main_window_media.dart';
import 'main_window_playback.dart';
import 'main_window_test_hooks.dart';

class MainWindowActionCoordinator {
  final ActionRegistry actionRegistry;
  final NativePlayerController controller;
  final MainWindowPlaybackCoordinator playbackCoordinator;
  final MainWindowMediaCoordinator mediaCoordinator;
  final MainWindowLayoutCoordinator layoutCoordinator;
  final MainWindowAnalysisCoordinator analysisCoordinator;
  final MainWindowTestHarness testHarness;
  final bool Function() isLoopRangeEnabled;
  final void Function() showMediaInfoOverlay;
  final void Function() showProfilerOverlay;
  final void Function() showSettingsDialog;
  final void Function() toggleFullScreen;
  final void Function() exitFullScreen;
  final SessionCapabilities Function() capabilities;
  final FutureOr<void> Function(int fileId) removeTrack;

  MainWindowActionBinder? _binder;

  MainWindowActionCoordinator({
    required this.actionRegistry,
    required this.controller,
    required this.playbackCoordinator,
    required this.mediaCoordinator,
    required this.layoutCoordinator,
    required this.analysisCoordinator,
    required this.testHarness,
    required this.isLoopRangeEnabled,
    required this.showMediaInfoOverlay,
    required this.showProfilerOverlay,
    required this.showSettingsDialog,
    required this.toggleFullScreen,
    required this.exitFullScreen,
    required this.capabilities,
    required this.removeTrack,
  });

  void bind() {
    _binder?.unbind();
    _binder = MainWindowActionBinder(
      actionRegistry: actionRegistry,
      capabilities: capabilities,
      togglePlayPause: playbackCoordinator.togglePlayPause,
      play: playbackCoordinator.play,
      pause: playbackCoordinator.pause,
      stepForward: playbackCoordinator.stepForward,
      stepBackward: playbackCoordinator.stepBackward,
      seekTo: playbackCoordinator.seekToAndWait,
      clickTimelineFraction: testHarness.clickTimelineFraction,
      setSpeed: playbackCoordinator.setSpeed,
      openFile: mediaCoordinator.openFile,
      addMediaByPath: mediaCoordinator.addMediaByPath,
      addNetworkMedia: mediaCoordinator.addNetworkMedia,
      addSshRemoteMedia: mediaCoordinator.addSshRemoteMedia,
      removeTrack: removeTrack,
      swapMediaHeader: mediaCoordinator.onMediaSwapped,
      adjustTrackOffset: mediaCoordinator.onOffsetChangedForSlot,
      setLoopRangeEnabled: playbackCoordinator.setLoopRangeEnabled,
      isLoopRangeEnabled: isLoopRangeEnabled,
      setLoopRange:
          (
            startUs,
            endUs, {
            seekToStart = false,
            seekOnlyIfStartChanged = false,
          }) => playbackCoordinator.setLoopRange(
            startUs,
            endUs,
            seekToStart: seekToStart,
            seekOnlyIfStartChanged: seekOnlyIfStartChanged,
          ),
      dragLoopHandle: testHarness.dragLoopHandle,
      dragSplitHandle: testHarness.dragSplitHandle,
      toggleLayoutMode: layoutCoordinator.toggleLayoutMode,
      setLayoutMode: layoutCoordinator.setLayoutMode,
      setZoom: layoutCoordinator.setZoom,
      setSplitPos: layoutCoordinator.setSplitPos,
      panByDelta: layoutCoordinator.panByDelta,
      toggleFullScreen: toggleFullScreen,
      exitFullScreen: exitFullScreen,
      openSettings: showSettingsDialog,
      openMediaInfo: showMediaInfoOverlay,
      openStats: showProfilerOverlay,
      openMemory: showProfilerOverlay,
      runAnalysis: analysisCoordinator.triggerAnalysis,
    )..bind();
  }

  void dispose() {
    _binder?.unbind();
    _binder = null;
  }
}

class MainWindowActionBinder {
  final ActionRegistry actionRegistry;
  final SessionCapabilities Function() capabilities;
  final void Function() togglePlayPause;
  final Future<void> Function() play;
  final Future<void> Function() pause;
  final Future<void> Function() stepForward;
  final Future<void> Function() stepBackward;
  final Future<void> Function(int ptsUs) seekTo;
  final void Function(double fraction) clickTimelineFraction;
  final void Function(double speed) setSpeed;

  final FutureOr<void> Function() openFile;
  final void Function(String path) addMediaByPath;
  final FutureOr<void> Function(String url) addNetworkMedia;
  final FutureOr<void> Function(String remotePath) addSshRemoteMedia;
  final FutureOr<void> Function(int fileId) removeTrack;
  final void Function(int slotIndex, int targetTrackIndex) swapMediaHeader;
  final FutureOr<void> Function(int slot, int deltaMs) adjustTrackOffset;
  final FutureOr<void> Function(bool enabled) setLoopRangeEnabled;
  final bool Function() isLoopRangeEnabled;
  final FutureOr<void> Function(
    int startUs,
    int endUs, {
    bool seekToStart,
    bool seekOnlyIfStartChanged,
  })
  setLoopRange;
  final void Function(String handle, int targetUs, {int steps}) dragLoopHandle;
  final void Function(double targetFraction, {int steps}) dragSplitHandle;

  final void Function() toggleLayoutMode;
  final void Function(int mode) setLayoutMode;
  final void Function(double ratio) setZoom;
  final void Function(double position) setSplitPos;
  final void Function(double dx, double dy) panByDelta;
  final void Function() toggleFullScreen;
  final void Function() exitFullScreen;

  final void Function() openSettings;
  final void Function() openMediaInfo;
  final void Function() openStats;
  final void Function() openMemory;
  final Future<void> Function() runAnalysis;

  final List<String> _boundActionNames = [];

  MainWindowActionBinder({
    required this.actionRegistry,
    required this.capabilities,
    required this.togglePlayPause,
    required this.play,
    required this.pause,
    required this.stepForward,
    required this.stepBackward,
    required this.seekTo,
    required this.clickTimelineFraction,
    required this.setSpeed,
    required this.openFile,
    required this.addMediaByPath,
    required this.addNetworkMedia,
    required this.addSshRemoteMedia,
    required this.removeTrack,
    required this.swapMediaHeader,
    required this.adjustTrackOffset,
    required this.setLoopRangeEnabled,
    required this.isLoopRangeEnabled,
    required this.setLoopRange,
    required this.dragLoopHandle,
    required this.dragSplitHandle,
    required this.toggleLayoutMode,
    required this.setLayoutMode,
    required this.setZoom,
    required this.setSplitPos,
    required this.panByDelta,
    required this.toggleFullScreen,
    required this.exitFullScreen,
    required this.openSettings,
    required this.openMediaInfo,
    required this.openStats,
    required this.openMemory,
    required this.runAnalysis,
  });

  void bind() {
    unbind();
    _bind(const TogglePlayPause(), (_) => togglePlayPause());
    _bind(const Play(), (_) => play());
    _bind(const Pause(), (_) => pause());
    _bind(const StepForward(), (_) => stepForward());
    _bind(const StepBackward(), (_) => stepBackward());
    _bind(const SeekTo(0), (action) {
      if (!capabilities().canSeek) return Future<void>.value();
      final a = action as SeekTo;
      return seekTo(a.ptsUs);
    });
    _bind(const ClickTimelineFraction(0), (action) {
      final a = action as ClickTimelineFraction;
      clickTimelineFraction(a.fraction);
    });
    _bind(const SetSpeed(1.0), (action) {
      final a = action as SetSpeed;
      setSpeed(a.speed);
    });

    _bind(const OpenFile(), (_) {
      final caps = capabilities();
      if (!caps.canOpenLocalMedia || !caps.canAddTrack) {
        return Future<void>.value();
      }
      return openFile();
    });
    _bind(const AddMedia(''), (action) {
      final caps = capabilities();
      if (!caps.canOpenLocalMedia || !caps.canAddTrack) return;
      final a = action as AddMedia;
      addMediaByPath(a.path);
    });
    _bind(const AddNetworkMedia(''), (action) {
      final caps = capabilities();
      if (!caps.canOpenNetworkMedia || !caps.canAddTrack) {
        return Future<void>.value();
      }
      final a = action as AddNetworkMedia;
      return addNetworkMedia(a.url);
    });
    _bind(const AddSshMedia(''), (action) {
      final caps = capabilities();
      if (!caps.canOpenSshMedia || !caps.canAddTrack) {
        return Future<void>.value();
      }
      final a = action as AddSshMedia;
      return addSshRemoteMedia(a.remotePath);
    });
    _bind(const RemoveTrackAction(0), (action) {
      if (!capabilities().canRemoveTrack) return Future<void>.value();
      final a = action as RemoveTrackAction;
      return removeTrack(a.fileId);
    });
    _bind(const SwapMediaHeader(0, 0), (action) {
      if (!capabilities().canReorderTrack) return;
      final a = action as SwapMediaHeader;
      swapMediaHeader(a.slotIndex, a.targetTrackIndex);
    });
    _bind(const AdjustTrackOffset(0, 0), (action) {
      if (!capabilities().canAdjustTrackOffset) {
        return Future<void>.value();
      }
      final a = action as AdjustTrackOffset;
      return adjustTrackOffset(a.slot, a.deltaMs);
    });
    _bind(const SetLoopEnabled(false), (action) {
      final a = action as SetLoopEnabled;
      return setLoopRangeEnabled(a.enabled);
    });
    _bind(const SetLoopRange(0, 0), (action) {
      final a = action as SetLoopRange;
      return setLoopRange(
        a.startUs,
        a.endUs,
        seekToStart: isLoopRangeEnabled(),
        seekOnlyIfStartChanged: true,
      );
    });
    _bind(const DragLoopHandle('end', 0), (action) {
      final a = action as DragLoopHandle;
      dragLoopHandle(a.handle, a.targetUs, steps: a.steps);
    });
    _bind(const DragSplitHandle(0.5), (action) {
      final a = action as DragSplitHandle;
      dragSplitHandle(a.targetFraction, steps: a.steps);
    });

    _bind(const ToggleLayoutMode(), (_) {
      if (!capabilities().canChangeViewMode) return;
      toggleLayoutMode();
    });
    _bind(const SetLayoutMode(0), (action) {
      if (!capabilities().canChangeViewMode) return;
      final a = action as SetLayoutMode;
      setLayoutMode(a.mode);
    });
    _bind(const SetZoom(1.0), (action) {
      if (!capabilities().canZoomViewport) return;
      final a = action as SetZoom;
      setZoom(a.ratio);
    });
    _bind(const SetSplitPos(0.5), (action) {
      final a = action as SetSplitPos;
      setSplitPos(a.position);
    });
    _bind(const Pan(0, 0), (action) {
      if (!capabilities().canPanViewport) return;
      final a = action as Pan;
      panByDelta(a.dx, a.dy);
    });
    _bind(const ToggleFullScreen(), (_) => toggleFullScreen());
    _bind(const ExitFullScreen(), (_) => exitFullScreen());

    _bind(const OpenSettings(), (_) => openSettings());
    _bind(const OpenMediaInfo(), (_) {
      if (!capabilities().canOpenMediaInfo) return;
      openMediaInfo();
    });
    _bind(const OpenStats(), (_) {
      if (!capabilities().canOpenProfiler) return;
      openStats();
    });
    _bind(const OpenMemory(), (_) {
      if (!capabilities().canOpenProfiler) return;
      openMemory();
    });
    _bind(const RunAnalysis(), (_) {
      if (!capabilities().canRunAnalysis) return Future<void>.value();
      return runAnalysis();
    });
  }

  void unbind() {
    for (final name in _boundActionNames.reversed) {
      actionRegistry.unbind(name);
    }
    _boundActionNames.clear();
  }

  void _bind(PlayerAction action, ActionCallback callback) {
    actionRegistry.bind(action, callback);
    _boundActionNames.add(action.name);
  }
}
