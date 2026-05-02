import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/video_renderer_controller.dart';

void main() {
  group('LayoutState', () {
    test('fromMap accepts numeric MethodChannel values', () {
      final state = LayoutState.fromMap({
        'mode': 1,
        'splitPos': 1,
        'zoomRatio': 2,
        'viewOffsetX': 3,
        'viewOffsetY': 4,
        'order': [3, 2, 1, 0],
      });

      expect(state.mode, LayoutMode.splitScreen);
      expect(state.splitPos, 1.0);
      expect(state.zoomRatio, 2.0);
      expect(state.viewOffsetX, 3.0);
      expect(state.viewOffsetY, 4.0);
      expect(state.order, [3, 2, 1, 0]);
    });

    test('equality compares order contents', () {
      expect(
        const LayoutState(order: [0, 2, 1, 3]),
        const LayoutState(order: [0, 2, 1, 3]),
      );
      expect(
        const LayoutState(order: [0, 2, 1, 3]),
        isNot(const LayoutState(order: [0, 1, 2, 3])),
      );
    });
  });
}
