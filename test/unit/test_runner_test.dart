import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/action_registry.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/automation/test_runner.dart';
import 'package:void_player/automation/ui_automation_bridge.dart';
import 'package:void_player/automation/ui_automation_runtime.dart';
import 'package:void_player/platform/analysis_process_host.dart';
import 'package:void_player/preferences/playback_preferences.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/windows/main/main_window_test_hooks.dart';

void main() {
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
    toggleAnalysisOverlayForSlot: (_) async {},
    toggleAnalysisOverlayPanel: () async {},
    setAnalysisOverlayType: (_) {},
    setAnalysisOverlayLayers: (_) {},
    setAnalysisOverlayOpacity: (_) {},
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

  @override
  Stream<NativePlayerEvent> get events => const Stream.empty();

  @override
  Future<CreatePlayerResult> createPlayer({
    required List<String> videoPaths,
    required int width,
    required int height,
    required bool useHardwareDecode,
  }) async {
    calls.add('createPlayer:${width}x$height:${videoPaths.join('|')}');
    return const CreatePlayerResult(textureId: 1, tracks: []);
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
  Future<void> stepForward() async {}

  @override
  Future<void> stepBackward() async {}

  @override
  Future<int> currentPts() async => 0;

  @override
  Future<PresentedFrameTiming?> currentPresentedFrame(int fileId) async => null;

  @override
  Future<int> duration() async => 0;

  @override
  Future<bool> isPlaying() async => false;

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
  Future<Map<String, dynamic>> getDiagnostics() async => const {};
}
