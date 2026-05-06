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

  test('fill view mode fits each track to its own slot', () {
    const tracks = [
      DisplayTrackGeometry(fileId: 1, width: 1280, height: 720),
      DisplayTrackGeometry(fileId: 2, width: 1920, height: 1080),
    ];

    final uniform = computeDisplayPixelSizeForLayout(
      viewportWidth: 1920,
      viewportHeight: 1080,
      layout: const LayoutState(order: [1, 2]),
      tracks: tracks,
    );
    expect(uniform.width, closeTo(640, 1e-9));
    expect(uniform.height, closeTo(360, 1e-9));

    final fillView = computeDisplayPixelSizeForLayout(
      viewportWidth: 1920,
      viewportHeight: 1080,
      layout: const LayoutState(
        order: [1, 2],
        pixelSizeMode: LayoutPixelSizeMode.fillView,
      ),
      tracks: tracks,
    );
    expect(fillView.width, 960);
    expect(fillView.height, 540);

    final selected1080p = computeDisplayPixelSizeForLayout(
      viewportWidth: 1920,
      viewportHeight: 1080,
      layout: const LayoutState(
        order: [2, 1],
        pixelSizeMode: LayoutPixelSizeMode.fillView,
      ),
      tracks: tracks,
    );
    expect(selected1080p.width, 960);
    expect(selected1080p.height, 540);
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
