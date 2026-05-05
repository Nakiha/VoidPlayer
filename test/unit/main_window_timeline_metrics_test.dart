import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/windows/main/main_window_state.dart';
import 'package:void_player/windows/main/main_window_timeline_metrics.dart';

TrackInfo _track(int fileId, int durationUs) => TrackInfo(
  fileId: fileId,
  slot: fileId,
  path: 'track_$fileId.mp4',
  width: 1920,
  height: 1080,
  durationUs: durationUs,
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
}
