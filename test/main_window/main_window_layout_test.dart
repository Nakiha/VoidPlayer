import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/main_window/main_window_layout.dart';
import 'package:void_player/main_window/main_window_state.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/viewport/display_geometry.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUpAll(() async {
    await initLogging(['--log-level=flutter=OFF']);
  });

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
        ..setPlayerIdentity(playerId: 1, textureId: 1)
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

      expect(controller.calls, const ['applyLayout', 'resize', 'getLayout']);
      expect(controller.resizes, const [Size(200, 150)]);
      expect(controller.appliedLayouts, hasLength(1));
      expect(controller.appliedLayouts.single.viewOffsetX, 20);
      expect(controller.appliedLayouts.single.viewOffsetY, 30);
      expect(stateStore.value.layout.viewOffsetX, 40);
      expect(stateStore.value.layout.viewOffsetY, 45);
    },
  );

  testWidgets('window resize debounces native compositor resize', (
    tester,
  ) async {
    final stateStore = MainWindowStateStore()
      ..setPlayerIdentity(playerId: 1, textureId: 1)
      ..setNativeCompositorActive(true)
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

    coordinator.onViewportResize(240, 180, 1.5);
    coordinator.onViewportResize(300, 220, 1.5);

    expect(controller.resizes, isEmpty);

    await tester.pump(MainWindowLayoutCoordinator.viewportResizeDebounce);
    await coordinator.flushPendingLayout();

    expect(controller.calls, const ['resize', 'getLayout']);
    expect(controller.resizes, const [Size(300, 220)]);
  });

  test(
    'preempt viewport resize lets native resize rescale applied pending layout',
    () async {
      final stateStore = MainWindowStateStore()
        ..setPlayerIdentity(playerId: 1, textureId: 1)
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

      expect(controller.calls, const ['applyLayout', 'resize', 'getLayout']);
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

  test('queued logical viewport width delta preempts native resize', () async {
    final stateStore = MainWindowStateStore()
      ..setPlayerIdentity(playerId: 1, textureId: 1)
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
    coordinator.viewportWidth = 400;
    coordinator.viewportHeight = 200;
    coordinator.viewportDevicePixelRatio = 2;

    coordinator.requestPreemptViewportLogicalSizeDelta(widthDelta: -30);
    await pumpEventQueue();

    expect(controller.resizes, const [Size(340, 200)]);
    expect(coordinator.viewportWidth, 340);
    expect(coordinator.viewportHeight, 200);
  });

  test('native compositor preempt resize refreshes native target layout', () async {
    final stateStore = MainWindowStateStore()
      ..setPlayerIdentity(playerId: 1, textureId: 1)
      ..setNativeCompositorActive(true)
      ..setLayout(const LayoutState(order: [1, -1, -1, -1]));
    addTearDown(stateStore.dispose);
    final trackManager = TrackManager()..setTracks([track(1)]);
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
    coordinator.viewportWidth = 2196;
    coordinator.viewportHeight = 876;
    controller.currentSize = const Size(2196, 876);

    await coordinator.preemptViewportResize(width: 1516, height: 876);

    expect(controller.resizes, const [Size(1516, 876)]);
    expect(controller.calls, const ['resize', 'getLayout']);
    expect(coordinator.viewportWidth, 1516);
    expect(coordinator.viewportHeight, 876);
  });

  test('visible marks sidebar width changes preempt native resize', () async {
    final stateStore = MainWindowStateStore()
      ..setPlayerIdentity(playerId: 1, textureId: 1)
      ..setMarksSidebarVisible(true)
      ..setMarksSidebarWidth(340)
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
    coordinator.viewportWidth = 1000;
    coordinator.viewportHeight = 500;
    coordinator.viewportDevicePixelRatio = 2;
    controller.currentSize = const Size(1000, 500);

    coordinator.setMarksSidebarWidth(400);
    await pumpEventQueue();

    expect(stateStore.value.marksSidebarWidth, 400);
    expect(controller.resizes, const [Size(880, 500)]);
    expect(coordinator.viewportWidth, 880);
    expect(coordinator.viewportHeight, 500);
  });

  testWidgets(
    'hiding marks sidebar removes Flutter panel before native resize',
    (tester) async {
      final stateStore = MainWindowStateStore()
        ..setPlayerIdentity(playerId: 1, textureId: 1)
        ..setMarksSidebarVisible(true)
        ..setMarksSidebarWidth(320)
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
      coordinator.viewportWidth = 1000;
      coordinator.viewportHeight = 500;
      coordinator.viewportDevicePixelRatio = 2;
      controller.currentSize = const Size(1000, 500);

      final visibleNotifications = <bool>[];
      stateStore.addListener(() {
        visibleNotifications.add(stateStore.value.marksSidebarVisible);
      });

      coordinator.setMarksSidebarVisible(false);

      expect(visibleNotifications, isNotEmpty);
      expect(visibleNotifications.first, isFalse);
      expect(stateStore.value.marksSidebarVisible, isFalse);
      expect(controller.resizes, isEmpty);

      tester.binding.scheduleFrame();
      await tester.pump();

      expect(controller.resizes, const [Size(1640, 500)]);
      expect(coordinator.viewportWidth, 1640);
      expect(coordinator.viewportHeight, 500);
    },
  );

  test('zoom combo changes are clamped through shared zoom path', () {
    final stateStore = MainWindowStateStore()
      ..setPlayerIdentity(playerId: 1, textureId: 1)
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

  test('pan submits the complete renderer layout', () async {
    final stateStore = MainWindowStateStore()
      ..setPlayerIdentity(playerId: 1, textureId: 1)
      ..setLayout(const LayoutState());
    addTearDown(stateStore.dispose);
    final trackManager = TrackManager()..setTracks([track(1)]);
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

    coordinator.panByDelta(24, -12);
    await coordinator.flushPendingLayout();

    expect(controller.calls, ['applyLayout']);
    expect(controller.appliedLayouts.single.viewOffsetX, 24);
    expect(controller.appliedLayouts.single.viewOffsetY, -12);
  });

  test('layout mode changes keep normalized view center stable', () {
    final stateStore = MainWindowStateStore()
      ..setPlayerIdentity(playerId: 1, textureId: 1)
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

  test('focus quick mark centers a rectangle with visual padding', () {
    final stateStore = MainWindowStateStore()
      ..setPlayerIdentity(playerId: 1, textureId: 1)
      ..setLayout(const LayoutState(order: [1, 2, -1, -1]));
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
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 1, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.2, 0.3, 0.4, 0.5),
    );

    coordinator.focusQuickMark(mark);

    final layout = stateStore.value.layout;
    expect(layout.zoomRatio, greaterThan(1));
    final projection = computeViewportLayoutProjection(
      viewportWidth: coordinator.viewportWidth,
      viewportHeight: coordinator.viewportHeight,
      layout: layout,
      tracks: trackManager.entries
          .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry.info))
          .toList(),
    );
    final projected = projection.viewportProjectionForSourceRect(
      mark.fileId,
      mark.sourceRect,
    );
    expect(projected, isNotNull);
    expect(projected!.viewportRect.center.dx, closeTo(400, 0.001));
    expect(projected.viewportRect.center.dy, closeTo(450, 0.001));
    expect(projected.viewportRect.width, lessThan(800));
    expect(projected.viewportRect.height, lessThan(900));
  });

  test('focus quick mark centers an arrow on its head', () {
    final stateStore = MainWindowStateStore()
      ..setPlayerIdentity(playerId: 1, textureId: 1)
      ..setLayout(const LayoutState(order: [1, 2, -1, -1]));
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
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 1, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.2, 0.2, 0.7, 0.6),
      sourceStart: Offset(0.2, 0.2),
      sourceEnd: Offset(0.7, 0.6),
      shape: QuickMarkShape.arrow,
    );

    coordinator.focusQuickMark(mark);

    final layout = stateStore.value.layout;
    final projection = computeViewportLayoutProjection(
      viewportWidth: coordinator.viewportWidth,
      viewportHeight: coordinator.viewportHeight,
      layout: layout,
      tracks: trackManager.entries
          .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry.info))
          .toList(),
    );
    final projected = projection.viewportProjectionForSourceRect(
      mark.fileId,
      mark.sourceRect,
    );
    expect(projected, isNotNull);
    final arrowHead = _sourcePointToViewportRect(
      projected!.viewportRect,
      mark.sourceRect,
      mark.effectiveSourceEnd,
    );
    expect(arrowHead.dx, closeTo(400, 0.001));
    expect(arrowHead.dy, closeTo(450, 0.001));
  });
}

Offset _sourcePointToViewportRect(
  Rect viewportRect,
  Rect sourceRect,
  Offset sourcePoint,
) {
  final tx = sourceRect.width.abs() <= 1e-6
      ? 0.5
      : (sourcePoint.dx - sourceRect.left) / sourceRect.width;
  final ty = sourceRect.height.abs() <= 1e-6
      ? 0.5
      : (sourcePoint.dy - sourceRect.top) / sourceRect.height;
  return Offset(
    viewportRect.left + viewportRect.width * tx,
    viewportRect.top + viewportRect.height * ty,
  );
}

class _FakeNativePlayerController extends NativePlayerController {
  _FakeNativePlayerController({LayoutState? nativeLayout})
    : nativeLayout = nativeLayout ?? const LayoutState();

  final List<String> calls = [];
  final List<Size> resizes = [];
  final List<LayoutState> appliedLayouts = [];
  LayoutState nativeLayout;
  Size currentSize = const Size(100, 100);
  int getLayoutCalls = 0;

  @override
  Future<void> resize(int width, int height) async {
    calls.add('resize');
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
  Future<void> prewarmNativePresentationTargetSize(
    int width,
    int height,
  ) async {}

  @override
  Future<void> applyLayout(LayoutState state) async {
    calls.add('applyLayout');
    appliedLayouts.add(state);
    nativeLayout = state;
  }

  @override
  Future<void> setNativeAnalysisOverlay(Map<String, Object?> state) async {}

  @override
  Future<LayoutState> getLayout() async {
    calls.add('getLayout');
    getLayoutCalls++;
    return nativeLayout;
  }
}
