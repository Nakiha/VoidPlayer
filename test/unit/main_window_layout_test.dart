import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/viewport/display_geometry.dart';
import 'package:void_player/windows/main/main_window_layout.dart';
import 'package:void_player/windows/main/main_window_state.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  TrackInfo track(int fileId) => TrackInfo(
    fileId: fileId,
    slot: fileId - 1,
    path: 'track_$fileId.mp4',
    width: 1920,
    height: 1080,
  );

  Offset normalizedViewCenter(
    MainWindowStateStore stateStore,
    TrackManager trackManager,
    int viewportWidth,
    int viewportHeight,
  ) {
    final layout = stateStore.value.layout;
    final display = computeDisplayPixelSizeForLayout(
      viewportWidth: viewportWidth,
      viewportHeight: viewportHeight,
      layout: layout,
      tracks: trackManager.entries
          .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry.info))
          .toList(),
    );
    return Offset(
      display.width.abs() > 1e-4 ? layout.viewOffsetX / display.width : 0,
      display.height.abs() > 1e-4 ? layout.viewOffsetY / display.height : 0,
    );
  }

  test(
    'immediate viewport resize applies pending layout before native resize',
    () async {
      final stateStore = MainWindowStateStore()
        ..setTextureId(1)
        ..setLayout(const LayoutState());
      addTearDown(stateStore.dispose);
      final trackManager = TrackManager();
      addTearDown(trackManager.dispose);
      final controller = _FakeNativePlayerController();
      final coordinator = MainWindowLayoutCoordinator(
        vsync: const TestVSync(),
        controller: controller,
        stateStore: stateStore,
        trackManager: trackManager,
        mounted: () => true,
      );
      addTearDown(coordinator.dispose);
      coordinator.viewportWidth = 100;
      coordinator.viewportHeight = 100;

      coordinator.panByDelta(20, 30);
      coordinator.onViewportResize(200, 150, 1, immediate: true);
      await coordinator.flushPendingLayout();

      expect(controller.events, const ['applyLayout', 'resize', 'getLayout']);
      expect(controller.resizes, const [Size(200, 150)]);
      expect(controller.appliedLayouts, hasLength(1));
      expect(controller.appliedLayouts.single.viewOffsetX, 20);
      expect(controller.appliedLayouts.single.viewOffsetY, 30);
      expect(stateStore.value.layout.viewOffsetX, 40);
      expect(stateStore.value.layout.viewOffsetY, 45);
    },
  );

  test(
    'preempt viewport resize lets native resize rescale applied pending layout',
    () async {
      final stateStore = MainWindowStateStore()
        ..setTextureId(1)
        ..setLayout(const LayoutState());
      addTearDown(stateStore.dispose);
      final trackManager = TrackManager();
      addTearDown(trackManager.dispose);
      final controller = _FakeNativePlayerController();
      final coordinator = MainWindowLayoutCoordinator(
        vsync: const TestVSync(),
        controller: controller,
        stateStore: stateStore,
        trackManager: trackManager,
        mounted: () => true,
      );
      addTearDown(coordinator.dispose);
      coordinator.viewportWidth = 100;
      coordinator.viewportHeight = 100;

      coordinator.panByDelta(20, 30);
      await coordinator.preemptViewportResize(width: 200, height: 150);

      expect(controller.events, const ['applyLayout', 'resize']);
      expect(controller.resizes, const [Size(200, 150)]);
      expect(controller.appliedLayouts, hasLength(1));
      expect(controller.appliedLayouts.single.viewOffsetX, 20);
      expect(controller.appliedLayouts.single.viewOffsetY, 30);
      expect(stateStore.value.layout.viewOffsetX, 40);
      expect(stateStore.value.layout.viewOffsetY, 45);
      expect(controller.nativeLayout.viewOffsetX, 40);
      expect(controller.nativeLayout.viewOffsetY, 45);
    },
  );

  test('zoom combo changes are clamped through shared zoom path', () {
    final stateStore = MainWindowStateStore()
      ..setTextureId(1)
      ..setLayout(const LayoutState());
    addTearDown(stateStore.dispose);
    final trackManager = TrackManager();
    addTearDown(trackManager.dispose);
    final coordinator = MainWindowLayoutCoordinator(
      vsync: const TestVSync(),
      controller: _FakeNativePlayerController(),
      stateStore: stateStore,
      trackManager: trackManager,
      mounted: () => true,
    );
    addTearDown(coordinator.dispose);

    coordinator.onZoomComboChanged(1000);

    expect(stateStore.value.layout.zoomRatio, LayoutState.zoomMax);
  });

  test('layout mode changes keep normalized view center stable', () {
    final stateStore = MainWindowStateStore()
      ..setTextureId(1)
      ..setLayout(
        const LayoutState(
          zoomRatio: 2,
          viewOffsetX: 140,
          viewOffsetY: 90,
          order: [1, 2, -1, -1],
        ),
      );
    addTearDown(stateStore.dispose);
    final trackManager = TrackManager()..setTracks([track(1), track(2)]);
    addTearDown(trackManager.dispose);
    final coordinator = MainWindowLayoutCoordinator(
      vsync: const TestVSync(),
      controller: _FakeNativePlayerController(),
      stateStore: stateStore,
      trackManager: trackManager,
      mounted: () => true,
    );
    addTearDown(coordinator.dispose);
    coordinator.viewportWidth = 1600;
    coordinator.viewportHeight = 900;

    final before = normalizedViewCenter(
      stateStore,
      trackManager,
      coordinator.viewportWidth,
      coordinator.viewportHeight,
    );

    coordinator.setLayoutMode(LayoutMode.splitScreen);
    final afterSplit = normalizedViewCenter(
      stateStore,
      trackManager,
      coordinator.viewportWidth,
      coordinator.viewportHeight,
    );

    coordinator.setLayoutMode(LayoutMode.sideBySide);
    final afterSideBySide = normalizedViewCenter(
      stateStore,
      trackManager,
      coordinator.viewportWidth,
      coordinator.viewportHeight,
    );

    expect(afterSplit.dx, closeTo(before.dx, 1e-9));
    expect(afterSplit.dy, closeTo(before.dy, 1e-9));
    expect(afterSideBySide.dx, closeTo(before.dx, 1e-9));
    expect(afterSideBySide.dy, closeTo(before.dy, 1e-9));
  });
}

class _FakeNativePlayerController extends NativePlayerController {
  _FakeNativePlayerController({LayoutState? nativeLayout})
    : nativeLayout = nativeLayout ?? const LayoutState();

  final List<String> events = [];
  final List<Size> resizes = [];
  final List<LayoutState> appliedLayouts = [];
  LayoutState nativeLayout;
  Size currentSize = const Size(100, 100);
  int getLayoutCalls = 0;

  @override
  Future<void> resize(int width, int height) async {
    events.add('resize');
    final oldSize = currentSize;
    resizes.add(Size(width.toDouble(), height.toDouble()));
    if (oldSize.width > 0 && oldSize.height > 0) {
      nativeLayout = nativeLayout.copyWith(
        viewOffsetX: nativeLayout.viewOffsetX * width / oldSize.width,
        viewOffsetY: nativeLayout.viewOffsetY * height / oldSize.height,
      );
    }
    currentSize = Size(width.toDouble(), height.toDouble());
  }

  @override
  Future<void> applyLayout(LayoutState state) async {
    events.add('applyLayout');
    appliedLayouts.add(state);
    nativeLayout = state;
  }

  @override
  Future<LayoutState> getLayout() async {
    events.add('getLayout');
    getLayoutCalls++;
    return nativeLayout;
  }
}
