import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/viewport/display_geometry.dart';

void main() {
  test('computes selected track display size for layout mode', () {
    const tracks = [
      DisplayTrackGeometry(fileId: 1, width: 100, height: 100),
      DisplayTrackGeometry(fileId: 2, width: 200, height: 100),
    ];

    final sideBySide = computeDisplayPixelSizeForLayout(
      viewportWidth: 400,
      viewportHeight: 200,
      layout: const LayoutState(order: [2, 1]),
      tracks: tracks,
    );
    expect(sideBySide.width, 200);
    expect(sideBySide.height, 100);

    final splitScreen = computeDisplayPixelSizeForLayout(
      viewportWidth: 400,
      viewportHeight: 200,
      layout: const LayoutState(mode: LayoutMode.splitScreen, order: [2, 1]),
      tracks: tracks,
    );
    expect(splitScreen.width, 400);
    expect(splitScreen.height, 200);
  });

  test('falls back to viewport size without tracks', () {
    final display = computeDisplayPixelSizeForLayout(
      viewportWidth: 320,
      viewportHeight: 180,
      layout: const LayoutState(),
      tracks: const [],
    );

    expect(display.width, 320);
    expect(display.height, 180);
  });
}
