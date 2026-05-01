import 'dart:async';

import '../../actions/action_registry.dart';
import '../../actions/player_action.dart';
import '../../video_renderer_controller.dart';
import 'main_window_analysis.dart';
import 'main_window_layout.dart';
import 'main_window_media.dart';
import 'main_window_playback.dart';
import 'main_window_test_hooks.dart';

class MainWindowActionCoordinator {
  final NativePlayerController controller;
  final MainWindowPlaybackCoordinator playbackCoordinator;
  final MainWindowMediaCoordinator mediaCoordinator;
  final MainWindowLayoutCoordinator layoutCoordinator;
  final MainWindowAnalysisCoordinator analysisCoordinator;
  final MainWindowTestHarness testHarness;
  final bool Function() isLoopRangeEnabled;
  final void Function() showProfilerOverlay;
  final void Function() showSettingsDialog;

  MainWindowActionBinder? _binder;

  MainWindowActionCoordinator({
    required this.controller,
    required this.playbackCoordinator,
    required this.mediaCoordinator,
    required this.layoutCoordinator,
    required this.analysisCoordinator,
    required this.testHarness,
    required this.isLoopRangeEnabled,
    required this.showProfilerOverlay,
    required this.showSettingsDialog,
  });

  void bind() {
    _binder?.unbind();
    _binder = MainWindowActionBinder(
      togglePlayPause: playbackCoordinator.togglePlayPause,
      play: playbackCoordinator.play,
      pause: playbackCoordinator.pause,
      stepForward: controller.stepForward,
      stepBackward: controller.stepBackward,
      seekTo: playbackCoordinator.seekTo,
      clickTimelineFraction: testHarness.clickTimelineFraction,
      setSpeed: playbackCoordinator.setSpeed,
      openFile: mediaCoordinator.openFile,
      addMediaByPath: mediaCoordinator.addMediaByPath,
      removeTrack: mediaCoordinator.removeTrack,
      adjustTrackOffset: mediaCoordinator.onOffsetChanged,
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
      openNewWindow: showProfilerOverlay,
      openSettings: showSettingsDialog,
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
  final void Function() togglePlayPause;
  final Future<void> Function() play;
  final Future<void> Function() pause;
  final Future<void> Function() stepForward;
  final Future<void> Function() stepBackward;
  final void Function(int ptsUs) seekTo;
  final void Function(double fraction) clickTimelineFraction;
  final void Function(double speed) setSpeed;

  final FutureOr<void> Function() openFile;
  final void Function(String path) addMediaByPath;
  final FutureOr<void> Function(int fileId) removeTrack;
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

  final void Function() openNewWindow;
  final void Function() openSettings;
  final void Function() openStats;
  final void Function() openMemory;
  final Future<void> Function() runAnalysis;

  final List<String> _boundActionNames = [];

  MainWindowActionBinder({
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
    required this.removeTrack,
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
    required this.openNewWindow,
    required this.openSettings,
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
      final a = action as SeekTo;
      seekTo(a.ptsUs);
    });
    _bind(const ClickTimelineFraction(0), (action) {
      final a = action as ClickTimelineFraction;
      clickTimelineFraction(a.fraction);
    });
    _bind(const SetSpeed(1.0), (action) {
      final a = action as SetSpeed;
      setSpeed(a.speed);
    });

    _bind(const OpenFile(), (_) => openFile());
    _bind(const AddMedia(''), (action) {
      final a = action as AddMedia;
      addMediaByPath(a.path);
    });
    _bind(const RemoveTrackAction(0), (action) {
      final a = action as RemoveTrackAction;
      return removeTrack(a.fileId);
    });
    _bind(const AdjustTrackOffset(0, 0), (action) {
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

    _bind(const ToggleLayoutMode(), (_) => toggleLayoutMode());
    _bind(const SetLayoutMode(0), (action) {
      final a = action as SetLayoutMode;
      setLayoutMode(a.mode);
    });
    _bind(const SetZoom(1.0), (action) {
      final a = action as SetZoom;
      setZoom(a.ratio);
    });
    _bind(const SetSplitPos(0.5), (action) {
      final a = action as SetSplitPos;
      setSplitPos(a.position);
    });
    _bind(const Pan(0, 0), (action) {
      final a = action as Pan;
      panByDelta(a.dx, a.dy);
    });

    _bind(const NewWindow(), (_) => openNewWindow());
    _bind(const OpenSettings(), (_) => openSettings());
    _bind(const OpenStats(), (_) => openStats());
    _bind(const OpenMemory(), (_) => openMemory());
    _bind(const RunAnalysis(), (_) => runAnalysis());
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
