import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/video_renderer_controller.dart';
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
}
