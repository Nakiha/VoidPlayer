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
  });

  Future<void> setSeekAfterJumpBehavior(SeekAfterJumpBehavior behavior);

  Future<void> maximizeWindow();

  Future<void> restoreWindow();

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
  }) {
    return generateTestVideo(
      path: path,
      frames: frames,
      fps: fps,
      width: width,
      height: height,
    );
  }

  @override
  Future<void> setSeekAfterJumpBehavior(SeekAfterJumpBehavior behavior) async {
    AppConfig.instance.seekAfterJumpBehavior = behavior;
    await AppConfig.instance.save();
  }

  @override
  Future<void> maximizeWindow() => wm.windowManager.maximize();

  @override
  Future<void> restoreWindow() => wm.windowManager.restore();

  @override
  void quit(int exitCode) => exit(exitCode);
}
