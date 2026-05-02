import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';

void main() {
  TrackInfo track(int fileId) => TrackInfo(
    fileId: fileId,
    slot: fileId,
    path: 'track_$fileId.mp4',
    width: 1920,
    height: 1080,
  );

  List<int> orderOf(TrackManager manager) =>
      manager.entries.map((entry) => entry.fileId).toList();

  test('moveTrack handles downward ReorderableListView indices', () {
    final manager = TrackManager()
      ..setTracks([track(0), track(1), track(2), track(3)]);

    manager.moveTrack(0, 3);

    expect(orderOf(manager), [1, 2, 0, 3]);
  });

  test('moveTrack handles upward ReorderableListView indices', () {
    final manager = TrackManager()
      ..setTracks([track(0), track(1), track(2), track(3)]);

    manager.moveTrack(2, 0);

    expect(orderOf(manager), [2, 0, 1, 3]);
  });

  test('moveTrack clamps append and ignores invalid old index', () {
    final manager = TrackManager()..setTracks([track(0), track(1), track(2)]);

    manager.moveTrack(1, 99);
    expect(orderOf(manager), [0, 2, 1]);

    manager.moveTrack(-1, 0);
    manager.moveTrack(99, 0);
    expect(orderOf(manager), [0, 2, 1]);
  });
}
