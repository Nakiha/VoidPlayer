import 'dart:io';

import 'package:window_manager/window_manager.dart' as wm;

import '../config/app_config.dart';
import '../preferences/playback_preferences.dart';
import 'test_video_generator.dart';

abstract interface class UiAutomationRuntime {
  Future<void> generateVideo({
    required String path,
    required int frames,
    required int fps,
    required int width,
    required int height,
    int ptsOffsetUs = 0,
    bool withAudio = false,
  });

  Future<void> setSeekAfterJumpBehavior(SeekAfterJumpBehavior behavior);

  Future<void> setDecodeMode(DecodeMode mode);

  Future<void> setViewportPixelSizeMode(ViewportPixelSizeMode mode);

  Future<void> maximizeWindow();

  Future<void> restoreWindow();

  Future<void> closeMainWindow();

  void quit(int exitCode);
}

class DefaultUiAutomationRuntime implements UiAutomationRuntime {
  const DefaultUiAutomationRuntime();

  @override
  Future<void> generateVideo({
    required String path,
    required int frames,
    required int fps,
    required int width,
    required int height,
    int ptsOffsetUs = 0,
    bool withAudio = false,
  }) {
    return generateTestVideo(
      path: path,
      frames: frames,
      fps: fps,
      width: width,
      height: height,
      ptsOffsetUs: ptsOffsetUs,
      withAudio: withAudio,
    );
  }

  @override
  Future<void> setSeekAfterJumpBehavior(SeekAfterJumpBehavior behavior) async {
    AppConfig.instance.seekAfterJumpBehavior = behavior;
    await AppConfig.instance.save();
  }

  @override
  Future<void> setDecodeMode(DecodeMode mode) async {
    AppConfig.instance.decodeMode = mode;
    await AppConfig.instance.save();
  }

  @override
  Future<void> setViewportPixelSizeMode(ViewportPixelSizeMode mode) async {
    AppConfig.instance.viewportPixelSizeMode = mode;
    await AppConfig.instance.save();
  }

  @override
  Future<void> maximizeWindow() => wm.windowManager.maximize();

  @override
  Future<void> restoreWindow() => wm.windowManager.restore();

  @override
  Future<void> closeMainWindow() => wm.windowManager.close();

  @override
  void quit(int exitCode) => exit(exitCode);
}
