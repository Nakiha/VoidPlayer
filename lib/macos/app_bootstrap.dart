import 'dart:io';

import 'package:flutter/material.dart';
import 'package:window_manager/window_manager.dart';

import '../app.dart';
import '../app_log.dart';
import '../config/app_config.dart';
import '../platform/analysis_process_host.dart';
import '../platform/main_window_platform.dart';
import '../platform/main_window_shutdown.dart';
import '../platform/native_file_picker.dart';
import '../platform/platform_capabilities.dart';
import '../platform/system_accent_watcher.dart';
import '../platform/window_bootstrap_args.dart';
import '../platform/window_bounds_policy.dart';
import '../startup_options.dart';

Future<void> _applyInitialMainWindowBounds({
  required ({double width, double height})? testWindow,
}) async {
  if (testWindow != null) {
    await windowManager.setSize(Size(testWindow.width, testWindow.height));
    await windowManager.center();
    return;
  }

  final savedRect = AppConfig.instance.windowRect;
  if (isRestorableWindowRect(savedRect)) {
    await windowManager.setBounds(savedRect!);
    return;
  }

  await windowManager.setSize(kDefaultMainWindowSize);
  await windowManager.center();
}

class _CloseHandler with WindowListener {
  @override
  void onWindowClose() async {
    windowManager.removeListener(this);
    try {
      final bounds = await windowManager.getBounds();
      AppConfig.instance.windowRect = bounds;
      await AppConfig.instance.save();
    } catch (error, stack) {
      log.warning('[macOS] window state save failed', error, stack);
    }
    try {
      await MainWindowShutdownRegistry.closeGracefully();
    } catch (error, stack) {
      log.severe('[macOS] main window shutdown failed', error, stack);
    }
    await windowManager.setPreventClose(false);
    await windowManager.close();
  }
}

Future<void> runMacOSVoidPlayer(List<String> args) async {
  if (args.contains('--standalone-analysis')) {
    log.warning('[macOS] standalone analysis windows are not supported yet');
    exit(2);
  }

  String? testScriptPath;
  final silentUiTest = hasCliFlag(args, '--silent-ui-test');
  final scriptIdx = args.indexOf('--test-script');
  if (scriptIdx >= 0 && scriptIdx + 1 < args.length) {
    testScriptPath = args[scriptIdx + 1];
  }
  final testWindow = testScriptPath != null
      ? parseTestWindowHeader(testScriptPath)
      : null;

  final startupOptions = StartupOptions.parse(args);
  for (final warning in startupOptions.warnings) {
    log.warning(warning);
  }

  await AppConfig.initialize();
  final analysisProcesses = UnsupportedAnalysisProcessHost()
    ..silentUiTest = silentUiTest;

  await windowManager.ensureInitialized();
  await windowManager.setMinimumSize(kMinimumMainWindowSize);
  await _applyInitialMainWindowBounds(testWindow: testWindow);
  await windowManager.setPreventClose(true);
  windowManager.addListener(_CloseHandler());
  log.info('Application starting (macOS baseline), silentUiTest=$silentUiTest');
  runApp(
    VoidPlayerApp(
      accentColor: const Color(0xFF007AFF),
      analysisProcesses: analysisProcesses,
      platformCapabilities: PlatformCapabilities.macOSPhase1,
      systemAccentWatcherFactory: NoopSystemAccentWatcher.new,
      platformWindow: const WindowManagerMainWindowPlatform(),
      nativeFilePicker: const MethodChannelNativeFilePicker(),
      testScriptPath: testScriptPath,
      startupOptions: startupOptions,
    ),
  );
  await WidgetsBinding.instance.endOfFrame;
  await windowManager.show(inactive: silentUiTest);
}
