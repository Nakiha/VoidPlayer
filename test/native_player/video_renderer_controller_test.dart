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
        'pixelSizeMode': 1,
        'order': [3, 2, 1, 0],
      });

      expect(state.mode, LayoutMode.splitScreen);
      expect(state.splitPos, 1.0);
      expect(state.zoomRatio, 2.0);
      expect(state.viewOffsetX, 3.0);
      expect(state.viewOffsetY, 4.0);
      expect(state.pixelSizeMode, LayoutPixelSizeMode.fillView);
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

  group('native player payloads', () {
    test(
      'CreatePlayerResult accepts native player without Flutter texture',
      () {
        final result = CreatePlayerResult.fromMap({
          'playerId': 42,
          'tracks': [
            {
              'fileId': 7,
              'slot': 1,
              'path': r'D:\media\a.mp4',
              'width': 1920,
              'height': 1080,
              'durationUs': 123,
            },
          ],
        });

        expect(result.playerId, 42);
        expect(result.textureId, isNull);
        expect(result.tracks.single.fileId, 7);
        expect(result.tracks.single.durationUs, 123);
      },
    );

    test('CreatePlayerResult rejects invalid track list payload', () {
      expect(
        () =>
            CreatePlayerResult.fromMap({'playerId': 1, 'tracks': 'not-a-list'}),
        throwsA(isA<NativeProtocolException>()),
      );
    });
  });

  group('NativePlayerController', () {
    test('delegates playback commands through injected api', () async {
      final api = _FakeNativePlayerApi();
      final controller = NativePlayerController(api: api);

      await controller.createPlayer(['a.mp4'], width: 320, height: 180);
      await controller.play();
      await controller.seek(123);
      await controller.resize(640, 360);
      await controller.captureViewportRegion(
        x: 3,
        y: 5,
        width: 40,
        height: 20,
        maxSize: 64,
        outputPath: '/tmp/region.png',
      );
      await controller.destroyPlayerOnly();

      expect(api.calls, [
        'createPlayer:320x180:a.mp4',
        'play',
        'seek:123:null',
        'resize:640x360',
        'captureViewportRegion:3,5 40x20 max=64 /tmp/region.png',
        'destroyPlayer',
      ]);
    });

    test('keeps no-player commands as no-ops', () async {
      final api = _FakeNativePlayerApi();
      final controller = NativePlayerController(api: api);

      await controller.play();
      await controller.seek(123);

      expect(api.calls, isEmpty);
    });

    test('delegates playback snapshot after player creation', () async {
      final api = _FakeNativePlayerApi();
      final controller = NativePlayerController(api: api);

      await controller.createPlayer(['a.mp4'], width: 320, height: 180);
      final snapshot = await controller.getPlaybackSnapshot(
        includePresentedFrames: true,
      );

      expect(snapshot.currentPtsUs, 1234);
      expect(snapshot.presentedFrames[1]?.ptsUs, 1234);
      expect(api.calls, [
        'createPlayer:320x180:a.mp4',
        'getPlaybackSnapshot:true',
      ]);
    });

    test('passes cached viewport background into player creation', () async {
      final api = _FakeNativePlayerApi();
      final controller = NativePlayerController(api: api);

      await controller.setViewportBackgroundColor(0xFF202124);
      await controller.createPlayer(['a.mp4'], width: 320, height: 180);

      expect(api.calls, [
        'createPlayer:320x180:a.mp4:bg=4280295716',
        'setViewportBackgroundColor:4280295716',
      ]);
    });

    test('keeps disposed playback commands as no-ops', () async {
      final api = _FakeNativePlayerApi();
      final controller = NativePlayerController(api: api);

      await controller.createPlayer(['a.mp4'], width: 320, height: 180);
      await controller.dispose();
      await controller.play();
      await controller.seek(123);
      await controller.resize(640, 360);
      await controller.setViewportBackgroundColor(0xFF000000);

      expect(api.calls, ['createPlayer:320x180:a.mp4', 'destroyPlayer']);
    });
  });
}

class _FakeNativePlayerApi implements NativePlayerApi {
  final calls = <String>[];

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
    final backgroundSuffix = viewportBackgroundColor == null
        ? ''
        : ':bg=$viewportBackgroundColor';
    calls.add(
      'createPlayer:${width}x$height:${videoPaths.join('|')}$backgroundSuffix',
    );
    return const CreatePlayerResult(playerId: 1, tracks: []);
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
  Future<void> resize({required int width, required int height}) async {
    calls.add('resize:${width}x$height');
  }

  @override
  Future<void> prewarmNativePresentationTargetSize({
    required int width,
    required int height,
  }) async {
    calls.add('prewarmNativePresentationTargetSize:${width}x$height');
  }

  @override
  Future<void> setNativeCompositorViewportRect({
    required int left,
    required int top,
    required int width,
    required int height,
    required int surfaceWidth,
    required int surfaceHeight,
  }) async {
    calls.add(
      'setNativeCompositorViewportRect:$left,$top ${width}x$height surface=${surfaceWidth}x$surfaceHeight',
    );
  }

  @override
  Future<void> requestNativeCompositorFlutterFrame({
    required String reason,
  }) async {
    calls.add('requestNativeCompositorFlutterFrame:$reason');
  }

  @override
  Future<void> boostNativeCompositorFlutterInteraction({
    required String reason,
  }) async {
    calls.add('boostNativeCompositorFlutterInteraction:$reason');
  }

  @override
  Future<void> ackNativeCompositorFlutterState({
    required int serial,
    required bool transparentViewport,
  }) async {
    calls.add('ackNativeCompositorFlutterState:$serial:$transparentViewport');
  }

  @override
  Future<void> debugFailNativeCompositor({required String reason}) async {
    calls.add('debugFailNativeCompositor:$reason');
  }

  @override
  Future<void> debugSimulateWindowsDeviceLoss({
    required String target,
    required String reason,
  }) async {
    calls.add('debugSimulateWindowsDeviceLoss:$target:$reason');
  }

  @override
  Future<void> resetNativePerfCounters() async {
    calls.add('resetNativePerfCounters');
  }

  @override
  Future<void> beginNativeInteractionSample({required String label}) async {
    calls.add('beginNativeInteractionSample:$label');
  }

  @override
  Future<void> endNativeInteractionSample({required String label}) async {
    calls.add('endNativeInteractionSample:$label');
  }

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
  }) async {
    calls.add('prepareNativeCompositorSourceCache:${sourceSlots.join('|')}');
  }

  @override
  Future<void> setNativeAnalysisOverlay(Map<String, Object?> state) async {}

  @override
  Future<void> clearNativeCompositorSourceCache({
    required String reason,
  }) async {
    calls.add('clearNativeCompositorSourceCache:$reason');
  }

  @override
  Future<void> setViewportBackgroundColor(int colorValue) async {
    calls.add('setViewportBackgroundColor:$colorValue');
  }

  @override
  Future<ViewportCapture> captureViewport({String? outputPath}) async {
    calls.add('captureViewport:$outputPath');
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
    calls.add(
      'captureViewportRegion:$x,$y ${width}x$height max=$maxSize $outputPath',
    );
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
    calls.add('captureWindow:$outputPath');
    return const ViewportCapture(
      hash: 'window-hash',
      width: 1,
      height: 1,
      avgLuma: 1,
      nonBlackRatio: 1,
    );
  }

  @override
  Future<Map<String, dynamic>> debugFlutterSurfaceInfo() async {
    calls.add('debugFlutterSurfaceInfo');
    return const {};
  }

  @override
  Future<Map<String, dynamic>> debugNativeCompositor() async {
    calls.add('debugNativeCompositor');
    return const {};
  }

  @override
  Future<void> stepForward() async {
    calls.add('stepForward');
  }

  @override
  Future<void> stepBackward() async {
    calls.add('stepBackward');
  }

  @override
  Future<int> currentPts() async {
    calls.add('currentPts');
    return 0;
  }

  @override
  Future<PresentedFrameTiming?> currentPresentedFrame(int fileId) async {
    calls.add('currentPresentedFrame:$fileId');
    return null;
  }

  @override
  Future<int> duration() async {
    calls.add('duration');
    return 0;
  }

  @override
  Future<bool> isPlaying() async {
    calls.add('isPlaying');
    return false;
  }

  @override
  Future<PlaybackSnapshot> getPlaybackSnapshot({
    bool includePresentedFrames = false,
  }) async {
    calls.add('getPlaybackSnapshot:$includePresentedFrames');
    return PlaybackSnapshot(
      currentPtsUs: 1234,
      durationUs: 5000,
      isPlaying: false,
      presentedFrames: includePresentedFrames
          ? const {1: PresentedFrameTiming(ptsUs: 1234, dtsUs: 1200)}
          : const {},
    );
  }

  @override
  Future<void> applyLayout(LayoutState state) async {
    calls.add('applyLayout');
  }

  @override
  Future<LayoutState> getLayout() async {
    calls.add('getLayout');
    return const LayoutState();
  }

  @override
  Future<TrackInfo> addTrack(
    String videoPath, {
    required bool useHardwareDecode,
  }) async {
    calls.add('addTrack:$videoPath');
    return TrackInfo(fileId: 1, slot: 0, path: videoPath, width: 1, height: 1);
  }

  @override
  Future<void> removeTrack(int fileId) async {
    calls.add('removeTrack:$fileId');
  }

  @override
  Future<void> setTrackOffset({
    required int fileId,
    required int offsetUs,
  }) async {
    calls.add('setTrackOffset:$fileId:$offsetUs');
  }

  @override
  Future<List<TrackInfo>> getTracks() async {
    calls.add('getTracks');
    return const [];
  }

  @override
  Future<Map<String, dynamic>> getDiagnostics() async {
    calls.add('getDiagnostics');
    return const {};
  }
}
