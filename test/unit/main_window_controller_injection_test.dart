import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/action_registry.dart';
import 'package:void_player/startup_options.dart';
import 'package:void_player/windows/main/main_window_controller.dart';
import 'package:void_player/windows/main/main_window_platform.dart';
import 'package:void_player/windows/window_manager.dart' as app_window;

class _FakeMainWindowPlatform implements MainWindowPlatform {
  @override
  Future<Rect> getBounds() => Future.value(Rect.zero);

  @override
  Future<void> setFullScreen(bool fullScreen) => Future.value();
}

void main() {
  test('MainWindowController keeps injected platform services', () {
    final platformWindow = _FakeMainWindowPlatform();
    final analysisProcesses = app_window.AnalysisProcessManager();
    final controller = MainWindowController(
      actionRegistry: ActionRegistry(),
      vsync: const TestVSync(),
      startupOptions: const StartupOptions(),
      mounted: () => true,
      platformWindow: platformWindow,
      analysisProcesses: analysisProcesses,
    );
    addTearDown(controller.dispose);

    expect(controller.platformWindow, same(platformWindow));
    expect(controller.analysisProcesses, same(analysisProcesses));
  });
}
