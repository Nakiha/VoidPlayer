import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/windows/main/main_window_layout.dart';
import 'package:void_player/windows/main/main_window_state.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

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
