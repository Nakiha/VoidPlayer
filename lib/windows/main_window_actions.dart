import 'dart:async';

import '../actions/action_registry.dart';
import '../actions/player_action.dart';
import '../video_renderer_controller.dart';
import 'main_window_analysis.dart';
import 'main_window_layout.dart';
import 'main_window_media.dart';
import 'main_window_playback.dart';
import 'main_window_test_hooks.dart';
import 'window_manager.dart';

class MainWindowActionCoordinator {
  final VideoRendererController controller;
  final MainWindowPlaybackCoordinator playbackCoordinator;
  final MainWindowMediaCoordinator mediaCoordinator;
  final MainWindowLayoutCoordinator layoutCoordinator;
  final MainWindowAnalysisCoordinator analysisCoordinator;
  final MainWindowTestHarness testHarness;
  final bool Function() isLoopRangeEnabled;

  const MainWindowActionCoordinator({
    required this.controller,
    required this.playbackCoordinator,
    required this.mediaCoordinator,
    required this.layoutCoordinator,
    required this.analysisCoordinator,
    required this.testHarness,
    required this.isLoopRangeEnabled,
  });

  void bind() {
    MainWindowActionBinder(
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
      setLoopRangeEnabled: (enabled) =>
          unawaited(playbackCoordinator.setLoopRangeEnabled(enabled)),
      isLoopRangeEnabled: isLoopRangeEnabled,
      setLoopRange:
          (
            startUs,
            endUs, {
            seekToStart = false,
            seekOnlyIfStartChanged = false,
          }) => unawaited(
            playbackCoordinator.setLoopRange(
              startUs,
              endUs,
              seekToStart: seekToStart,
              seekOnlyIfStartChanged: seekOnlyIfStartChanged,
            ),
          ),
      dragLoopHandle: testHarness.dragLoopHandle,
      toggleLayoutMode: layoutCoordinator.toggleLayoutMode,
      setLayoutMode: layoutCoordinator.setLayoutMode,
      setZoom: layoutCoordinator.setZoom,
      setSplitPos: layoutCoordinator.setSplitPos,
      panByDelta: layoutCoordinator.panByDelta,
      openNewWindow: WindowManager.showStatsWindow,
      openSettings: WindowManager.showSettingsWindow,
      openStats: WindowManager.showStatsWindow,
      openMemory: WindowManager.showMemoryWindow,
      runAnalysis: analysisCoordinator.triggerAnalysis,
    ).bind();
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

  final void Function() openFile;
  final void Function(String path) addMediaByPath;
  final void Function(int fileId) removeTrack;
  final void Function(int slot, int deltaMs) adjustTrackOffset;
  final void Function(bool enabled) setLoopRangeEnabled;
  final bool Function() isLoopRangeEnabled;
  final void Function(
    int startUs,
    int endUs, {
    bool seekToStart,
    bool seekOnlyIfStartChanged,
  })
  setLoopRange;
  final void Function(String handle, int targetUs, {int steps}) dragLoopHandle;

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

  const MainWindowActionBinder({
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
    actionRegistry.bind(const TogglePlayPause(), (_) => togglePlayPause());
    actionRegistry.bind(const Play(), (_) => play());
    actionRegistry.bind(const Pause(), (_) => pause());
    actionRegistry.bind(const StepForward(), (_) => stepForward());
    actionRegistry.bind(const StepBackward(), (_) => stepBackward());
    actionRegistry.bind(const SeekTo(0), (action) {
      final a = action as SeekTo;
      seekTo(a.ptsUs);
    });
    actionRegistry.bind(const ClickTimelineFraction(0), (action) {
      final a = action as ClickTimelineFraction;
      clickTimelineFraction(a.fraction);
    });
    actionRegistry.bind(const SetSpeed(1.0), (action) {
      final a = action as SetSpeed;
      setSpeed(a.speed);
    });

    actionRegistry.bind(const OpenFile(), (_) => openFile());
    actionRegistry.bind(const AddMedia(''), (action) {
      final a = action as AddMedia;
      addMediaByPath(a.path);
    });
    actionRegistry.bind(const RemoveTrackAction(0), (action) {
      final a = action as RemoveTrackAction;
      removeTrack(a.fileId);
    });
    actionRegistry.bind(const AdjustTrackOffset(0, 0), (action) {
      final a = action as AdjustTrackOffset;
      adjustTrackOffset(a.slot, a.deltaMs);
    });
    actionRegistry.bind(const SetLoopEnabled(false), (action) {
      final a = action as SetLoopEnabled;
      setLoopRangeEnabled(a.enabled);
    });
    actionRegistry.bind(const SetLoopRange(0, 0), (action) {
      final a = action as SetLoopRange;
      setLoopRange(
        a.startUs,
        a.endUs,
        seekToStart: isLoopRangeEnabled(),
        seekOnlyIfStartChanged: true,
      );
    });
    actionRegistry.bind(const DragLoopHandle('end', 0), (action) {
      final a = action as DragLoopHandle;
      dragLoopHandle(a.handle, a.targetUs, steps: a.steps);
    });

    actionRegistry.bind(const ToggleLayoutMode(), (_) => toggleLayoutMode());
    actionRegistry.bind(const SetLayoutMode(0), (action) {
      final a = action as SetLayoutMode;
      setLayoutMode(a.mode);
    });
    actionRegistry.bind(const SetZoom(1.0), (action) {
      final a = action as SetZoom;
      setZoom(a.ratio);
    });
    actionRegistry.bind(const SetSplitPos(0.5), (action) {
      final a = action as SetSplitPos;
      setSplitPos(a.position);
    });
    actionRegistry.bind(const Pan(0, 0), (action) {
      final a = action as Pan;
      panByDelta(a.dx, a.dy);
    });

    actionRegistry.bind(const NewWindow(), (_) => openNewWindow());
    actionRegistry.bind(const OpenSettings(), (_) => openSettings());
    actionRegistry.bind(const OpenStats(), (_) => openStats());
    actionRegistry.bind(const OpenMemory(), (_) => openMemory());
    actionRegistry.bind(const RunAnalysis(), (_) => runAnalysis());
  }
}
