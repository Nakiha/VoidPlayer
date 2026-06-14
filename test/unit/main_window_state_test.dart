import 'dart:ui';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/main_window/main_window_media.dart';
import 'package:void_player/main_window/main_window_playback.dart';
import 'package:void_player/main_window/main_window_state.dart';
import 'package:void_player/main_window/main_window_timeline_metrics.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/marks/quick_mark_store.dart';
import 'package:void_player/preferences/playback_preferences.dart';
import 'package:void_player/startup_options.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/viewport/viewport_display_state.dart';
import 'package:void_player/widgets/controls_bar.dart';

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

  test('marks sidebar width is clamped to supported range', () {
    final store = MainWindowStateStore();
    addTearDown(store.dispose);

    store.setMarksSidebarWidth(kMinMarksSidebarWidth - 100);
    expect(store.value.marksSidebarWidth, kMinMarksSidebarWidth);

    store.setMarksSidebarWidth(kMaxMarksSidebarWidth + 100);
    expect(store.value.marksSidebarWidth, kMaxMarksSidebarWidth);
  });

  test('seek preview clears stale presented frame anchors', () {
    final store = MainWindowStateStore();
    addTearDown(store.dispose);

    store.setPolledPlaybackState(
      1000000,
      2000000,
      false,
      presentedFrameAnchors: const {
        1: QuickMarkAnchor(fileId: 1, ptsUs: 1000000, dtsUs: 1000000),
      },
    );

    store.setSeekPreview(1500000);

    expect(store.value.currentPtsUs, 1500000);
    expect(store.value.pendingSeekUs, 1500000);
    expect(store.value.presentedFrameAnchors, isEmpty);
  });

  testWidgets('pending seek suppresses stale presented frame anchors', (
    tester,
  ) async {
    final fixture = _PlaybackFixture();
    try {
      fixture.api.presentedFrameTiming = const PresentedFrameTiming(
        ptsUs: 1000000,
        dtsUs: 1000000,
      );
      await fixture.controller.createPlayer(
        ['clip.mp4'],
        width: 1920,
        height: 1080,
      );
      fixture.api.calls.clear();
      fixture.store.setTextureId(1);
      fixture.store.setQuickMarks(const [
        QuickMark(
          id: 1,
          anchor: QuickMarkAnchor(
            fileId: 1,
            ptsUs: 1500000,
            dtsUs: 1400000,
            sourcePacketIndex: 8,
            sourcePacketSize: 1024,
          ),
          sourceRect: Rect.fromLTRB(0.1, 0.1, 0.2, 0.2),
        ),
      ]);
      fixture.store.setPolledPlaybackState(
        1000000,
        fixture.metrics.effectiveDurationUs,
        false,
        presentedFrameAnchors: const {
          1: QuickMarkAnchor(fileId: 1, ptsUs: 1000000, dtsUs: 1000000),
        },
      );

      fixture.coordinator.seekTo(1500000);
      expect(fixture.store.value.currentPtsUs, 1500000);
      expect(fixture.store.value.pendingSeekUs, 1500000);
      expect(fixture.store.value.presentedFrameAnchors[1]?.ptsUs, 1500000);
      expect(
        QuickMarkStore(marks: fixture.store.value.quickMarks)
            .view(
              context: QuickMarkFrameContext(
                currentPtsUs: fixture.store.value.currentPtsUs,
                presentedFrameAnchors:
                    fixture.store.value.presentedFrameAnchors,
              ),
              selectedMarkId: 1,
            )
            .visibleMarkIds,
        const {1},
      );

      await tester.pump();
      fixture.coordinator.startPolling();
      await tester.pump(const Duration(milliseconds: 250));

      expect(fixture.api.calls, contains('getPlaybackSnapshot:true'));
      expect(fixture.api.calls, isNot(contains('currentPresentedFrame:1')));
      expect(fixture.store.value.currentPtsUs, 1500000);
      expect(fixture.store.value.pendingSeekUs, 1500000);
      expect(fixture.store.value.presentedFrameAnchors[1]?.ptsUs, 1500000);
    } finally {
      fixture.dispose();
    }
  });

  group('MainWindowPlaybackCoordinator loop range step', () {
    test('wraps forward step to loop start after crossing loop end', () async {
      final fixture = _PlaybackFixture(stepForwardPtsUs: 1600000);
      addTearDown(fixture.dispose);

      await fixture.enableLoopRange(startUs: 1000000, endUs: 1500000);
      await fixture.coordinator.stepForward();

      expect(fixture.api.calls, contains('stepForward'));
      expect(fixture.api.calls, contains('seek:1000000:2'));
      expect(fixture.api.ptsUs, 1000000);
      expect(fixture.store.value.currentPtsUs, 1000000);
    });

    test('clamps backward step to loop start below range', () async {
      final fixture = _PlaybackFixture(stepBackwardPtsUs: 900000);
      addTearDown(fixture.dispose);

      await fixture.enableLoopRange(startUs: 1000000, endUs: 1500000);
      await fixture.coordinator.stepBackward();

      expect(fixture.api.calls, contains('stepBackward'));
      expect(fixture.api.calls, contains('seek:1000000:2'));
      expect(fixture.api.ptsUs, 1000000);
      expect(fixture.store.value.currentPtsUs, 1000000);
    });
  });

  group('default audio policy', () {
    const firstTrack = TrackInfo(
      fileId: 1,
      slot: 0,
      path: 'clip.mp4',
      width: 1920,
      height: 1080,
    );

    test('keeps new tracks muted by default', () {
      expect(
        defaultAudibleTrackForPolicy(
          policy: DefaultAudioPlaybackPolicy.muted,
          currentAudibleTrack: null,
          addedTracks: const [firstTrack],
        ),
        isNull,
      );
    });

    test('can select the first added track by default', () {
      expect(
        defaultAudibleTrackForPolicy(
          policy: DefaultAudioPlaybackPolicy.playFirstTrack,
          currentAudibleTrack: null,
          addedTracks: const [firstTrack],
        ),
        1,
      );
    });

    test('keeps an existing audible track when adding muted-default media', () {
      expect(
        defaultAudibleTrackForPolicy(
          policy: DefaultAudioPlaybackPolicy.muted,
          currentAudibleTrack: 1,
          addedTracks: const [
            TrackInfo(
              fileId: 2,
              slot: 1,
              path: 'second.mp4',
              width: 1,
              height: 1,
            ),
          ],
        ),
        1,
      );
    });
  });
}

class _PlaybackFixture {
  final _PlaybackApi api;
  late final NativePlayerController controller;
  final MainWindowStateStore store = MainWindowStateStore();
  final TrackManager tracks = TrackManager();
  late final MainWindowTimelineMetrics metrics;
  late final MainWindowPlaybackCoordinator coordinator;

  _PlaybackFixture({int? stepForwardPtsUs, int? stepBackwardPtsUs})
    : api = _PlaybackApi(
        stepForwardPtsUs: stepForwardPtsUs,
        stepBackwardPtsUs: stepBackwardPtsUs,
      ) {
    controller = NativePlayerController(api: api);
    tracks.setTracks(const [
      TrackInfo(
        fileId: 1,
        slot: 0,
        path: 'clip.mp4',
        width: 1920,
        height: 1080,
        durationUs: 2000000,
      ),
    ]);
    metrics = MainWindowTimelineMetrics(
      stateStore: store,
      trackManager: tracks,
    );
    coordinator = MainWindowPlaybackCoordinator(
      controller: controller,
      trackManager: tracks,
      startupOptions: const StartupOptions(),
      stateStore: store,
      timelineHoverNotifier: TimelineHoverStateNotifier(),
      playbackPreferences: const _PlaybackPrefs(),
      mounted: () => true,
      timelineMetrics: metrics,
    );
  }

  Future<void> enableLoopRange({
    required int startUs,
    required int endUs,
  }) async {
    await controller.createPlayer(['clip.mp4'], width: 1920, height: 1080);
    store.setLoopRange(startUs, endUs);
    store.setLoopRangeEnabled(true);
    store.setPolledPlaybackState(startUs, metrics.effectiveDurationUs, false);
    api.ptsUs = startUs;
  }

  void dispose() {
    coordinator.dispose();
    store.dispose();
    tracks.dispose();
  }
}

class TimelineHoverStateNotifier extends ValueNotifier<TimelineHoverState> {
  TimelineHoverStateNotifier() : super(const TimelineHoverState());
}

class _PlaybackPrefs implements PlaybackPreferences {
  @override
  final DefaultAudioPlaybackPolicy defaultAudioPlaybackPolicy =
      DefaultAudioPlaybackPolicy.muted;

  @override
  final PerformanceAlertPolicy performanceAlertPolicy =
      PerformanceAlertPolicy.sustained;

  const _PlaybackPrefs();

  @override
  DecodeMode get decodeMode => DecodeMode.preferHardware;

  @override
  bool get useHardwareDecode => decodeMode.useHardwareDecode;

  @override
  SeekAfterJumpBehavior get seekAfterJumpBehavior =>
      SeekAfterJumpBehavior.keepPreviousState;

  @override
  ViewportPixelSizeMode get viewportPixelSizeMode =>
      ViewportPixelSizeMode.uniformVideoPixels;
}

class _PlaybackApi implements NativePlayerApi {
  final List<String> calls = [];
  final int? stepForwardPtsUs;
  final int? stepBackwardPtsUs;
  int ptsUs = 1000000;
  PresentedFrameTiming? presentedFrameTiming;

  _PlaybackApi({this.stepForwardPtsUs, this.stepBackwardPtsUs});

  @override
  Stream<NativePlayerEvent> get events => const Stream.empty();

  @override
  Future<CreatePlayerResult> createPlayer({
    required List<String> videoPaths,
    required int width,
    required int height,
    required bool useHardwareDecode,
    int? viewportBackgroundColor,
  }) async {
    calls.add('createPlayer');
    return const CreatePlayerResult(
      textureId: 1,
      tracks: [
        TrackInfo(
          fileId: 1,
          slot: 0,
          path: 'clip.mp4',
          width: 1920,
          height: 1080,
          durationUs: 2000000,
        ),
      ],
    );
  }

  @override
  Future<void> destroyPlayer() async {
    calls.add('destroyPlayer');
  }

  @override
  Future<void> play() async {
    calls.add('play');
  }

  @override
  Future<void> pause() async {
    calls.add('pause');
  }

  @override
  Future<void> seek(int ptsUs, {int? requestId}) async {
    calls.add('seek:$ptsUs:$requestId');
    this.ptsUs = ptsUs;
  }

  @override
  Future<void> setSpeed(double speed) async {
    calls.add('setSpeed:$speed');
  }

  @override
  Future<void> setLoopRange({
    required bool enabled,
    required int startUs,
    required int endUs,
  }) async {
    calls.add('setLoopRange:$enabled:$startUs:$endUs');
  }

  @override
  Future<void> setAudibleTrack(int? fileId) async {
    calls.add('setAudibleTrack:$fileId');
  }

  @override
  Future<void> resize({required int width, required int height}) async {}

  @override
  Future<void> setNativeCompositorViewportRect({
    required int left,
    required int top,
    required int width,
    required int height,
    required int surfaceWidth,
    required int surfaceHeight,
  }) async {}

  @override
  Future<void> setNativeCompositorViewportTransform({
    required bool enabled,
    required double scaleX,
    required double scaleY,
    required double translateX,
    required double translateY,
    required int mode,
    required double splitPos,
    required int activeTrackCount,
  }) async {}

  @override
  Future<void> prepareNativeCompositorSourceCache({
    required List<int> sourceSlots,
    required List<int> sourceOrder,
    required int mode,
    required double splitPos,
    required int activeTrackCount,
    required List<double> displayOffsetX,
    required List<double> displayOffsetY,
    required List<double> invDisplaySizeX,
    required List<double> invDisplaySizeY,
    required List<double> viewOffsetUvX,
    required List<double> viewOffsetUvY,
  }) async {}

  @override
  Future<void> setNativeAnalysisOverlay(Map<String, Object?> state) async {}

  @override
  Future<void> clearNativeCompositorSourceCache({
    required String reason,
  }) async {}

  @override
  Future<void> setViewportBackgroundColor(int colorValue) async {}

  @override
  Future<ViewportCapture> captureViewport({String? outputPath}) async {
    return const ViewportCapture(
      hash: 'hash',
      width: 1,
      height: 1,
      avgLuma: 1,
      nonBlackRatio: 1,
    );
  }

  @override
  Future<ViewportCapture> captureViewportRegion({
    required int x,
    required int y,
    required int width,
    required int height,
    required int maxSize,
    String? outputPath,
  }) async {
    return ViewportCapture(
      hash: 'region-hash',
      width: width,
      height: height,
      avgLuma: 1,
      nonBlackRatio: 1,
      outputPath: outputPath,
    );
  }

  @override
  Future<ViewportCapture> captureWindow({String? outputPath}) async {
    return const ViewportCapture(
      hash: 'window-hash',
      width: 1,
      height: 1,
      avgLuma: 1,
      nonBlackRatio: 1,
    );
  }

  @override
  Future<Map<String, dynamic>> debugFlutterSurfaceInfo() async => const {};

  @override
  Future<Map<String, dynamic>> debugNativeCompositor() async => const {};

  @override
  Future<void> stepForward() async {
    calls.add('stepForward');
    ptsUs = stepForwardPtsUs ?? ptsUs;
  }

  @override
  Future<void> stepBackward() async {
    calls.add('stepBackward');
    ptsUs = stepBackwardPtsUs ?? ptsUs;
  }

  @override
  Future<int> currentPts() async {
    calls.add('currentPts');
    return ptsUs;
  }

  @override
  Future<PresentedFrameTiming?> currentPresentedFrame(int fileId) async {
    calls.add('currentPresentedFrame:$fileId');
    return presentedFrameTiming;
  }

  @override
  Future<int> duration() async => 2000000;

  @override
  Future<bool> isPlaying() async => false;

  @override
  Future<PlaybackSnapshot> getPlaybackSnapshot({
    bool includePresentedFrames = false,
  }) async {
    calls.add('getPlaybackSnapshot:$includePresentedFrames');
    return PlaybackSnapshot(
      currentPtsUs: ptsUs,
      durationUs: 2000000,
      isPlaying: false,
      presentedFrames: includePresentedFrames && presentedFrameTiming != null
          ? {1: presentedFrameTiming!}
          : const {},
    );
  }

  @override
  Future<void> applyLayout(LayoutState state) async {}

  @override
  Future<LayoutState> getLayout() async => const LayoutState();

  @override
  Future<TrackInfo> addTrack(
    String videoPath, {
    required bool useHardwareDecode,
  }) async {
    return TrackInfo(fileId: 2, slot: 1, path: videoPath, width: 1, height: 1);
  }

  @override
  Future<void> removeTrack(int fileId) async {}

  @override
  Future<void> setTrackOffset({
    required int fileId,
    required int offsetUs,
  }) async {}

  @override
  Future<List<TrackInfo>> getTracks() async => const [];

  @override
  Future<Map<String, dynamic>> getDiagnostics() async => const {};
}
