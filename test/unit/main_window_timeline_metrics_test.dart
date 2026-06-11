import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/main_window/main_window_state.dart';
import 'package:void_player/main_window/main_window_timeline_metrics.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';

TrackInfo _track(int fileId, int durationUs, {int startTimeUs = 0}) =>
    TrackInfo(
      fileId: fileId,
      slot: fileId,
      path: 'track_$fileId.mp4',
      width: 1920,
      height: 1080,
      durationUs: durationUs,
      startTimeUs: startTimeUs,
    );

void main() {
  test('effective duration includes per-track sync offsets', () {
    final stateStore = MainWindowStateStore()
      ..setPolledPlaybackState(1, 1000, false)
      ..setSyncOffsets({2: 700});
    addTearDown(stateStore.dispose);
    final trackManager = TrackManager()
      ..setTracks([_track(1, 1200), _track(2, 1500)]);
    addTearDown(trackManager.dispose);

    final metrics = MainWindowTimelineMetrics(
      stateStore: stateStore,
      trackManager: trackManager,
    );

    expect(metrics.effectiveDurationUs, 2200);
  });

  test('negative initial PTS offset can shrink controls duration', () {
    final stateStore = MainWindowStateStore()
      ..setPolledPlaybackState(1, 4000, false)
      ..setSyncOffsets({1: -2000});
    addTearDown(stateStore.dispose);
    final trackManager = TrackManager()..setTracks([_track(1, 4000)]);
    addTearDown(trackManager.dispose);

    final metrics = MainWindowTimelineMetrics(
      stateStore: stateStore,
      trackManager: trackManager,
    );

    expect(metrics.effectiveDurationUs, 2000);
  });

  test(
    'auto initial PTS offset maps non-zero PTS range to content duration',
    () {
      final stateStore = MainWindowStateStore()
        ..setPolledPlaybackState(1, 4000, false)
        ..setSyncOffsets({1: -2000});
      addTearDown(stateStore.dispose);
      final trackManager = TrackManager()
        ..setTracks([_track(1, 4000, startTimeUs: 2000)]);
      addTearDown(trackManager.dispose);

      final metrics = MainWindowTimelineMetrics(
        stateStore: stateStore,
        trackManager: trackManager,
      );

      expect(metrics.effectiveDurationUs, 4000);
    },
  );

  test(
    'absolute-end FLV duration shrinks to playable span after PTS offset',
    () {
      const startUs = 10196998000;
      const absoluteEndUs = 10378832000;
      final stateStore = MainWindowStateStore()
        ..setPolledPlaybackState(1, absoluteEndUs, false)
        ..setSyncOffsets({1: -startUs});
      addTearDown(stateStore.dispose);
      final trackManager = TrackManager()
        ..setTracks([_track(1, absoluteEndUs, startTimeUs: startUs)]);
      addTearDown(trackManager.dispose);

      final metrics = MainWindowTimelineMetrics(
        stateStore: stateStore,
        trackManager: trackManager,
      );

      expect(metrics.effectiveDurationUs, 181834000);
    },
  );

  test('falls back to polled duration until track metadata is available', () {
    final stateStore = MainWindowStateStore()
      ..setPolledPlaybackState(1, 4000, false);
    addTearDown(stateStore.dispose);
    final trackManager = TrackManager();
    addTearDown(trackManager.dispose);

    final metrics = MainWindowTimelineMetrics(
      stateStore: stateStore,
      trackManager: trackManager,
    );

    expect(metrics.effectiveDurationUs, 4000);
  });
}
