import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/action_registry.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/automation/main_window_harness.dart';
import 'package:void_player/automation/test_runner.dart';
import 'package:void_player/automation/ui_automation_bridge.dart';
import 'package:void_player/automation/ui_automation_runtime.dart';
import 'package:void_player/platform/analysis_process_host.dart';
import 'package:void_player/preferences/playback_preferences.dart';
import 'package:void_player/video_renderer_controller.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUpAll(() async {
    await initLogging(['--log-level=flutter=OFF']);
  });

  group('TestRunner', () {
    test('QUIT skips native destroy when no player exists', () async {
      final api = _FakeNativePlayerApi();
      final runtime = _FakeRuntime();
      final runner = TestRunner(
        scriptPath: _writeScript('0.0,QUIT,0\n'),
        automation: _bridge(NativePlayerController(api: api)),
        runtime: runtime,
      );

      await runner.run();

      expect(api.calls, isEmpty);
      expect(runtime.quitCodes, [0]);
    });

    test('QUIT destroys an existing native player before exit', () async {
      final api = _FakeNativePlayerApi();
      final controller = NativePlayerController(api: api);
      await controller.createPlayer(['a.mp4'], width: 320, height: 180);

      final runtime = _FakeRuntime();
      final runner = TestRunner(
        scriptPath: _writeScript('0.0,QUIT,0\n'),
        automation: _bridge(controller),
        runtime: runtime,
      );

      await runner.run();

      expect(api.calls, ['createPlayer:320x180:a.mp4', 'destroyPlayer']);
      expect(runtime.quitCodes, [0]);
    });

    test('native diagnostic int-at-least assertions pass', () async {
      final api = _FakeNativePlayerApi(
        diagnostics: const {'nativeCompositorEDRVideoMaxRGBX1000': 3979},
      );
      final controller = NativePlayerController(api: api);
      await controller.createPlayer(['a.mp4'], width: 320, height: 180);

      final runtime = _FakeRuntime();
      final runner = TestRunner(
        scriptPath: _writeScript('''
0.0,ASSERT_NATIVE_DIAGNOSTIC_INT_AT_LEAST,nativeCompositorEDRVideoMaxRGBX1000,1001
0.1,QUIT,0
'''),
        automation: _bridge(controller),
        runtime: runtime,
      );

      await runner.run();

      expect(runtime.quitCodes, [0]);
    });

    test('native diagnostic int-at-least assertions fail', () async {
      final api = _FakeNativePlayerApi(
        diagnostics: const {'nativeCompositorEDRVideoMaxRGBX1000': 1000},
      );
      final controller = NativePlayerController(api: api);
      await controller.createPlayer(['a.mp4'], width: 320, height: 180);

      final runtime = _FakeRuntime();
      final runner = TestRunner(
        scriptPath: _writeScript('''
0.0,ASSERT_NATIVE_DIAGNOSTIC_INT_AT_LEAST,nativeCompositorEDRVideoMaxRGBX1000,1001
0.1,QUIT,0
'''),
        automation: _bridge(controller),
        runtime: runtime,
      );

      await runner.run();

      expect(runtime.quitCodes, [1]);
    });

    test('forced native compositor failure reaches the native API', () async {
      final api = _FakeNativePlayerApi();
      final controller = NativePlayerController(api: api);
      await controller.createPlayer(['a.mp4'], width: 320, height: 180);

      final runtime = _FakeRuntime();
      final runner = TestRunner(
        scriptPath: _writeScript('''
0.0,DEBUG_FAIL_NATIVE_COMPOSITOR,contract-test
0.1,QUIT,0
'''),
        automation: _bridge(controller),
        runtime: runtime,
      );

      await runner.run();

      expect(api.calls, contains('debugFailNativeCompositor:contract-test'));
      expect(runtime.quitCodes, [0]);
    });

    test('simulated Windows device loss reaches the native API', () async {
      final api = _FakeNativePlayerApi();
      final controller = NativePlayerController(api: api);
      await controller.createPlayer(['a.mp4'], width: 320, height: 180);

      final runtime = _FakeRuntime();
      final runner = TestRunner(
        scriptPath: _writeScript('''
0.0,DEBUG_SIMULATE_WINDOWS_DEVICE_LOSS,compositor,contract-test
0.1,QUIT,0
'''),
        automation: _bridge(controller),
        runtime: runtime,
      );

      await runner.run();

      expect(
        api.calls,
        contains('debugSimulateWindowsDeviceLoss:compositor:contract-test'),
      );
      expect(runtime.quitCodes, [0]);
    });

    test('wait presented frame range polls until frame enters range', () async {
      final api = _FakeNativePlayerApi(
        presentedFrames: [
          const PresentedFrameTiming(ptsUs: 100000, dtsUs: 100000),
          const PresentedFrameTiming(ptsUs: 1200000, dtsUs: 1200000),
        ],
      );
      final controller = NativePlayerController(api: api);
      await controller.createPlayer(['a.mp4'], width: 320, height: 180);

      final runtime = _FakeRuntime();
      final runner = TestRunner(
        scriptPath: _writeScript('''
0.0,WAIT_PRESENTED_FRAME_RANGE,1,900000,1500000,1000,1
0.1,QUIT,0
'''),
        automation: _bridge(controller),
        runtime: runtime,
      );

      await runner.run();

      expect(api.currentPresentedFrameCalls, 2);
      expect(runtime.quitCodes, [0]);
    });

    test('wait presented frame range fails after timeout', () async {
      final api = _FakeNativePlayerApi(
        presentedFrames: [
          const PresentedFrameTiming(ptsUs: 100000, dtsUs: 100000),
        ],
      );
      final controller = NativePlayerController(api: api);
      await controller.createPlayer(['a.mp4'], width: 320, height: 180);

      final runtime = _FakeRuntime();
      final runner = TestRunner(
        scriptPath: _writeScript('''
0.0,WAIT_PRESENTED_FRAME_RANGE,1,900000,1500000,5,1
0.1,QUIT,0
'''),
        automation: _bridge(controller),
        runtime: runtime,
      );

      await runner.run();

      expect(runtime.quitCodes, [1]);
    });
  });
}

String _writeScript(String contents) {
  final file = File(
    '${Directory.systemTemp.path}${Platform.pathSeparator}'
    'void_player_test_runner_${DateTime.now().microsecondsSinceEpoch}.csv',
  );
  file.writeAsStringSync(contents);
  addTearDown(() {
    if (file.existsSync()) file.deleteSync();
  });
  return file.path;
}

UiAutomationBridge _bridge(NativePlayerController controller) {
  return UiAutomationBridge(
    controller: controller,
    analysisProcesses: UnsupportedAnalysisProcessHost(),
    testHarness: MainWindowTestHarness(
      viewportKey: GlobalKey(),
      timelineSliderKey: GlobalKey(),
      controlsBarKey: GlobalKey(),
      analysisOverlayButtonKey: GlobalKey(),
      fullFrameCaptureKey: GlobalKey(),
      loopRangeBarKey: GlobalKey(),
      splitPosition: () => 0.5,
      timelineStartWidth: () => 0,
      effectiveDurationUs: () => 0,
      resolvedLoopStartUs: () => 0,
      resolvedLoopEndUs: () => 0,
    ),
    effectiveDurationUs: () => 0,
    timelinePtsUs: () => 0,
    toggleAnalysisOverlayForSlot: (_) async {},
    toggleAnalysisOverlayPanel: () async {},
    toggleMarksSidebar: () {},
    generateAnalysisCacheForSlot: (_) async => 'hash',
    setMediaSourceIdForSlot: (_, _) async {},
    exportMarksToFile: (_) async {},
    addQuickMark: (_) async {},
    clearMarks: () {},
    quickMarkCount: () => 0,
    setAnalysisOverlayType: (_) {},
    setAnalysisOverlayLayers: (_) {},
    setAnalysisOverlayOpacity: (_) {},
    dartViewportDiagnostics: () => const {},
    actionRegistry: ActionRegistry(),
  );
}

class _FakeRuntime implements UiAutomationRuntime {
  final quitCodes = <int>[];

  @override
  Future<void> generateVideo({
    required String path,
    required int frames,
    required int fps,
    required int width,
    required int height,
    int ptsOffsetUs = 0,
    bool withAudio = false,
  }) async {}

  @override
  Future<void> setSeekAfterJumpBehavior(SeekAfterJumpBehavior behavior) async {}

  @override
  Future<void> setDecodeMode(DecodeMode mode) async {}

  @override
  Future<void> setViewportPixelSizeMode(ViewportPixelSizeMode mode) async {}

  @override
  Future<void> maximizeWindow() async {}

  @override
  Future<void> restoreWindow() async {}

  @override
  Future<void> closeMainWindow() async {}

  @override
  void quit(int exitCode) {
    quitCodes.add(exitCode);
  }
}

class _FakeNativePlayerApi implements NativePlayerApi {
  final calls = <String>[];
  final Map<String, dynamic> diagnostics;
  final List<PresentedFrameTiming?> presentedFrames;
  int currentPresentedFrameCalls = 0;

  _FakeNativePlayerApi({
    this.diagnostics = const {},
    this.presentedFrames = const [],
  });

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
    calls.add('createPlayer:${width}x$height:${videoPaths.join('|')}');
    return const CreatePlayerResult(playerId: 1, tracks: []);
  }

  @override
  Future<void> destroyPlayer() async {
    calls.add('destroyPlayer');
  }

  @override
  Future<void> play() async {}

  @override
  Future<void> pause() async {}

  @override
  Future<void> seek(int ptsUs, {int? requestId}) async {}

  @override
  Future<void> setSpeed(double speed) async {}

  @override
  Future<void> setLoopRange({
    required bool enabled,
    required int startUs,
    required int endUs,
  }) async {}

  @override
  Future<void> setAudibleTrack(int? fileId) async {}

  @override
  Future<void> resize({required int width, required int height}) async {}

  @override
  Future<void> prewarmNativePresentationTargetSize({
    required int width,
    required int height,
  }) async {}

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
  }) async {}

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
  Future<void> stepForward() async {}

  @override
  Future<void> stepBackward() async {}

  @override
  Future<int> currentPts() async => 0;

  @override
  Future<PresentedFrameTiming?> currentPresentedFrame(int fileId) async {
    final index = currentPresentedFrameCalls++;
    if (presentedFrames.isEmpty) {
      return null;
    }
    if (index >= presentedFrames.length) {
      return presentedFrames.last;
    }
    return presentedFrames[index];
  }

  @override
  Future<int> duration() async => 0;

  @override
  Future<bool> isPlaying() async => false;

  @override
  Future<PlaybackSnapshot> getPlaybackSnapshot({
    bool includePresentedFrames = false,
  }) async {
    return const PlaybackSnapshot(
      currentPtsUs: 0,
      durationUs: 0,
      isPlaying: false,
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
    return TrackInfo(fileId: 1, slot: 0, path: videoPath, width: 1, height: 1);
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
  Future<Map<String, dynamic>> getDiagnostics() async => diagnostics;
}
