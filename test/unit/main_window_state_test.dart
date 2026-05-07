import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/viewport/viewport_display_state.dart';
import 'package:void_player/widgets/controls_bar.dart';
import 'package:void_player/windows/main/main_window_playback.dart';
import 'package:void_player/windows/main/main_window_state.dart';

void main() {
  test('MainWindowStateStore skips redundant notifications', () {
    final store = MainWindowStateStore();
    addTearDown(store.dispose);

    var notifications = 0;
    store.addListener(() => notifications++);

    store.setPlaying(false);
    store.setLoopRange(0, 0);
    store.setLayout(const LayoutState());
    store.setSyncOffsets(const {});

    expect(notifications, 0);

    store.setPlaying(true);
    store.setPlaying(true);
    store.setLoopRange(10, 20);
    store.setLoopRange(10, 20);

    expect(notifications, 2);
  });

  test('resetAfterLastTrackRemoved clears startup loop range state', () {
    final store = MainWindowStateStore();
    addTearDown(store.dispose);

    store.setStartupLoopRangeApplied(true);
    store.setLoopRangeEnabled(true);
    store.setNativeLoopRangeSynced(true);
    store.setLoopRange(1000000, 2000000);

    store.resetAfterLastTrackRemoved();

    expect(store.value.startupLoopRangeApplied, isFalse);
    expect(store.value.loopRangeEnabled, isFalse);
    expect(store.value.nativeLoopRangeSynced, isFalse);
    expect(store.value.loopStartUs, 0);
    expect(store.value.loopEndUs, 0);
  });

  test('MainWindowStateStore keeps typed viewport errors', () {
    final store = MainWindowStateStore();
    addTearDown(store.dispose);

    store.setViewportState(const ViewportDisplayState.error('no decoder'));

    expect(store.value.viewportState.status, ViewportDisplayStatus.error);
    expect(store.value.viewportState.errorText, 'no decoder');

    store.resetAfterLastTrackRemoved();

    expect(store.value.viewportState, const ViewportDisplayState.empty());
  });

  test(
    'default timeline splitter leaves room for all controls bar buttons',
    () {
      const state = MainWindowStateModel();
      final timelineStartWidth =
          MainWindowPlaybackCoordinator.trackDragHandleWidth +
          state.timelineControlsWidth +
          MainWindowPlaybackCoordinator.trackDividerWidth;

      expect(
        timelineStartWidth,
        greaterThanOrEqualTo(ControlsBar.minimumStartWidthForFullControls),
      );
    },
  );
}
