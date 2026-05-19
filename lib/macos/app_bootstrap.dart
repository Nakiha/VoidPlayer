import 'dart:io';

import 'package:flutter/material.dart';
import 'package:window_manager/window_manager.dart';

import '../app.dart';
import '../app_log.dart';
import '../config/app_config.dart';
import '../platform/analysis_process_host.dart';
import '../platform/main_window_platform.dart';
import '../platform/native_file_picker.dart';
import '../platform/platform_capabilities.dart';
import '../platform/system_accent_watcher.dart';
import '../startup_options.dart';

bool _hasFlag(List<String> args, String name) =>
    args.any((arg) => arg == name || arg.startsWith('$name='));

({double width, double height})? _parseTestWindowHeader(String scriptPath) {
  try {
    final file = File(scriptPath);
    if (!file.existsSync()) return null;
    for (final rawLine in file.readAsLinesSync()) {
      final line = rawLine.trim();
      if (!line.startsWith('@')) continue;
      final parts = line.split(',').map((s) => s.trim()).toList();
      if (parts.isEmpty) continue;
      final key = parts.first.toUpperCase();
      if (key == '@WINDOW' && parts.length >= 3) {
        return (width: double.parse(parts[1]), height: double.parse(parts[2]));
      }
    }
  } catch (_) {
    // Ignore malformed test header and fall back to normal config handling.
  }
  return null;
}

Future<void> _applyInitialMainWindowBounds({
  required ({double width, double height})? testWindow,
}) async {
  if (testWindow != null) {
    await windowManager.setSize(Size(testWindow.width, testWindow.height));
    await windowManager.center();
    return;
  }

  await windowManager.setSize(const Size(1280, 720));
  await windowManager.center();
}

Future<void> runMacOSVoidPlayer(List<String> args) async {
  if (args.contains('--standalone-analysis')) {
    log.warning('[macOS] standalone analysis windows are not supported yet');
    exit(2);
  }

  String? testScriptPath;
  final silentUiTest = _hasFlag(args, '--silent-ui-test');
  final scriptIdx = args.indexOf('--test-script');
  if (scriptIdx >= 0 && scriptIdx + 1 < args.length) {
    testScriptPath = args[scriptIdx + 1];
  }
  final testWindow = testScriptPath != null
      ? _parseTestWindowHeader(testScriptPath)
      : null;

  final startupOptions = StartupOptions.parse(args);
  for (final warning in startupOptions.warnings) {
    log.warning(warning);
  }

  await AppConfig.initialize();
  final analysisProcesses = UnsupportedAnalysisProcessHost()
    ..silentUiTest = silentUiTest;

  await windowManager.ensureInitialized();
  await windowManager.setMinimumSize(const Size(520, 360));
  await _applyInitialMainWindowBounds(testWindow: testWindow);
  await windowManager.show(inactive: silentUiTest);

  log.info('Application starting (macOS baseline), silentUiTest=$silentUiTest');
  runApp(
    VoidPlayerApp(
      accentColor: const Color(0xFF007AFF),
      analysisProcesses: analysisProcesses,
      platformCapabilities: PlatformCapabilities.macOSPhase1,
      systemAccentWatcherFactory: NoopSystemAccentWatcher.new,
      platformWindow: const WindowManagerMainWindowPlatform(),
      nativeFilePicker: const UnsupportedNativeFilePicker(),
      testScriptPath: testScriptPath,
      startupOptions: startupOptions,
    ),
  );
}
