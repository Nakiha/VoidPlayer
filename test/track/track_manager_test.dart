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

  test('swapTracks exchanges absolute display positions', () {
    final manager = TrackManager()..setTracks([track(0), track(1), track(2)]);

    manager.swapTracks(0, 2);
    expect(orderOf(manager), [2, 1, 0]);

    manager.swapTracks(1, 2);
    expect(orderOf(manager), [2, 0, 1]);

    manager.swapTracks(-1, 0);
    manager.swapTracks(0, 99);
    expect(orderOf(manager), [2, 0, 1]);
  });

  test('addTrack caps display order at maxTracks', () {
    final manager = TrackManager();

    for (var i = 0; i < TrackManager.maxTracks + 2; i++) {
      manager.addTrack(track(i));
    }

    expect(manager.count, TrackManager.maxTracks);
    expect(orderOf(manager), [0, 1, 2, 3]);
  });

  test('addTrack refreshes an existing file id instead of duplicating it', () {
    final manager = TrackManager()..addTrack(track(1));

    manager.addTrack(
      const TrackInfo(
        fileId: 1,
        slot: 0,
        path: 'replacement.mp4',
        width: 3840,
        height: 2160,
      ),
    );

    expect(manager.count, 1);
    expect(manager.entries.single.fileId, 1);
    expect(manager.entries.single.path, 'replacement.mp4');
  });

  test('setTracks keeps file ids unique', () {
    final manager = TrackManager();

    manager.setTracks([
      track(1),
      const TrackInfo(
        fileId: 1,
        slot: 0,
        path: 'replacement.mp4',
        width: 3840,
        height: 2160,
      ),
      track(2),
    ]);

    expect(orderOf(manager), [1, 2]);
    expect(manager.entries.first.path, 'replacement.mp4');
  });

  test('setTracks asserts when native returns too many tracks', () {
    final manager = TrackManager();

    expect(
      () => manager.setTracks([
        for (var i = 0; i < TrackManager.maxTracks + 1; i++) track(i),
      ]),
      throwsAssertionError,
    );
  });
}
