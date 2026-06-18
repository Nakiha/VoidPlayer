import 'dart:ui';

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

  test(
    'projects side-by-side viewport points to source uv by display order',
    () {
      const tracks = [
        DisplayTrackGeometry(fileId: 1, width: 100, height: 100),
        DisplayTrackGeometry(fileId: 2, width: 100, height: 100),
      ];
      final projection = computeViewportLayoutProjection(
        viewportWidth: 400,
        viewportHeight: 200,
        layout: const LayoutState(order: [2, 1]),
        tracks: tracks,
      );

      final left = projection.hitTestPhysical(const Offset(100, 100));
      expect(left?.fileId, 2);
      expect(left?.sourceUv.dx, closeTo(0.5, 1e-9));
      expect(left?.sourceUv.dy, closeTo(0.5, 1e-9));

      final right = projection.hitTestPhysical(const Offset(300, 100));
      expect(right?.fileId, 1);
      expect(right?.sourceUv.dx, closeTo(0.5, 1e-9));
      expect(right?.sourceUv.dy, closeTo(0.5, 1e-9));

      final rect = projection.viewportRectForSourceRect(
        1,
        Rect.fromLTRB(0.25, 0.25, 0.75, 0.75),
      );
      expect(rect?.left, closeTo(250, 1e-9));
      expect(rect?.top, closeTo(50, 1e-9));
      expect(rect?.right, closeTo(350, 1e-9));
      expect(rect?.bottom, closeTo(150, 1e-9));
    },
  );

  test('accounts for zoom and pan when hit testing source uv', () {
    const tracks = [DisplayTrackGeometry(fileId: 1, width: 100, height: 100)];
    final projection = computeViewportLayoutProjection(
      viewportWidth: 400,
      viewportHeight: 200,
      layout: const LayoutState(zoomRatio: 2, viewOffsetX: 200),
      tracks: tracks,
    );

    final hit = projection.hitTestPhysical(const Offset(200, 100));
    expect(hit?.fileId, 1);
    expect(hit?.sourceUv.dx, closeTo(0.0, 1e-9));
    expect(hit?.sourceUv.dy, closeTo(0.5, 1e-9));
  });

  test('clips source rect projection to split-screen visible region', () {
    const tracks = [
      DisplayTrackGeometry(fileId: 1, width: 100, height: 100),
      DisplayTrackGeometry(fileId: 2, width: 100, height: 100),
    ];
    final projection = computeViewportLayoutProjection(
      viewportWidth: 400,
      viewportHeight: 200,
      layout: const LayoutState(
        mode: LayoutMode.splitScreen,
        splitPos: 0.5,
        order: [1, 2],
      ),
      tracks: tracks,
    );

    final leftRect = projection.viewportRectForSourceRect(
      1,
      Rect.fromLTRB(0, 0, 1, 1),
    );
    expect(leftRect?.left, closeTo(100, 1e-9));
    expect(leftRect?.top, closeTo(0, 1e-9));
    expect(leftRect?.right, closeTo(200, 1e-9));
    expect(leftRect?.bottom, closeTo(200, 1e-9));

    final rightRect = projection.viewportRectForSourceRect(
      2,
      Rect.fromLTRB(0, 0, 1, 1),
    );
    expect(rightRect?.left, closeTo(200, 1e-9));
    expect(rightRect?.top, closeTo(0, 1e-9));
    expect(rightRect?.right, closeTo(300, 1e-9));
    expect(rightRect?.bottom, closeTo(200, 1e-9));

    final projectedLeft = projection.viewportProjectionForSourceRect(
      1,
      Rect.fromLTRB(0, 0, 1, 1),
    );
    expect(projectedLeft?.viewportRect.left, closeTo(100, 1e-9));
    expect(projectedLeft?.viewportRect.right, closeTo(300, 1e-9));
    expect(projectedLeft?.clipRect.left, closeTo(0, 1e-9));
    expect(projectedLeft?.clipRect.right, closeTo(200, 1e-9));
  });
}
