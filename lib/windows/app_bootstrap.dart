import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_acrylic/flutter_acrylic.dart';
import 'package:window_manager/window_manager.dart' hide WindowManager;

import '../app.dart';
import '../app_log.dart';
import '../config/app_config.dart';
import '../startup_options.dart';
import 'analysis/ipc/analysis_ipc_client.dart';
import 'analysis/analysis_window.dart';
import 'win32ffi.dart';
import 'window_manager.dart';

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

bool _hasFlag(List<String> args, String name) =>
    args.any((arg) => arg == name || arg.startsWith('$name='));

const MethodChannel _windowBootstrapChannel = MethodChannel(
  'void_player/window_bootstrap',
);

class _StartupTrace {
  final Stopwatch _total = Stopwatch()..start();
  final Stopwatch _step = Stopwatch()..start();

  void mark(String label) {
    log.info(
      '[Startup] $label: +${_step.elapsedMilliseconds}ms, '
      'total=${_total.elapsedMilliseconds}ms',
    );
    _step
      ..reset()
      ..start();
  }
}

class _StepTrace {
  final String name;
  final Stopwatch _total = Stopwatch()..start();
  final Stopwatch _step = Stopwatch()..start();

  _StepTrace(this.name);

  void mark(String label) {
    log.info(
      '[Startup:$name] $label: +${_step.elapsedMilliseconds}ms, '
      'total=${_total.elapsedMilliseconds}ms',
    );
    _step
      ..reset()
      ..start();
  }
}

Future<void> _showWindowForMode({required bool silent}) async {
  if (silent) {
    final hwnds = Win32FFI.findCurrentProcessWindowsByClass(kMainWindowClass);
    for (final hwnd in hwnds) {
      Win32FFI.hideFromTaskbar(hwnd);
    }
  }
  try {
    await _windowBootstrapChannel.invokeMethod<void>('showAfterNextFrame', {
      'inactive': silent,
    });
  } catch (error, stackTrace) {
    log.warning(
      '[WindowBootstrap] native frame-gated show failed; falling back',
      error,
      stackTrace,
    );
    await windowManager.show(inactive: silent);
  }
}

int _currentFlutterRunnerHwnd() {
  final hwnds = Win32FFI.findCurrentProcessWindowsByClass(kMainWindowClass);
  if (hwnds.isNotEmpty) return hwnds.first;
  final foregroundHwnd = Win32FFI.getForegroundWindow();
  return Win32FFI.isCurrentProcessWindowOfClass(
        foregroundHwnd,
        kMainWindowClass,
      )
      ? foregroundHwnd
      : 0;
}

Color _getWindowsAccentColor() => Color(Win32FFI.getDwmAccentColorArgb());

bool _isRestorableMainWindowRect(Rect? rect) {
  if (rect == null) return false;
  if (rect.width < 520 || rect.height < 360) return false;
  return Win32FFI.isRectOnScreen(rect);
}

Future<void> _showWindowForModeWithTrace({
  required bool silent,
  required _StartupTrace trace,
}) async {
  await _showWindowForMode(silent: silent);
  trace.mark('show requested');
}

Future<void> _applyInitialMainWindowBounds({
  required ({double width, double height})? testWindow,
}) async {
  final trace = _StepTrace('bounds');
  if (testWindow != null) {
    await _applyInitialMainWindowBoundsWithWindowManager(testWindow);
    trace.mark('test bounds applied');
    return;
  }

  if (_isRestorableMainWindowRect(AppConfig.instance.windowRect)) {
    // The runner consumes window_manager's saved logical bounds before Flutter
    // starts. Reapplying them here via Win32 would mix logical and physical
    // coordinate systems on non-100% DPI displays.
    trace.mark('restored bounds kept');
    return;
  }

  await _applyInitialMainWindowBoundsWithWindowManager(null);
  trace.mark('default bounds applied');
}

Future<void> _applyInitialMainWindowBoundsWithWindowManager(
  ({double width, double height})? testWindow,
) async {
  final savedRect = AppConfig.instance.windowRect;
  if (testWindow != null) {
    await windowManager.setSize(Size(testWindow.width, testWindow.height));
    await windowManager.center();
  } else if (savedRect != null && Win32FFI.isRectOnScreen(savedRect)) {
    await windowManager.setSize(savedRect.size);
    await windowManager.setPosition(savedRect.topLeft);
  } else {
    await windowManager.setSize(const Size(1280, 720));
    await windowManager.center();
  }
}

/// Handles the close button: saves window state, closes analysis processes,
/// then closes the main window.
class _CloseHandler with WindowListener {
  @override
  void onWindowClose() async {
    windowManager.removeListener(this);
    final bounds = await windowManager.getBounds();
    AppConfig.instance.windowRect = bounds;
    await AppConfig.instance.save();
    await WindowManager.closeAllAnalysisWindows();
    await windowManager.setPreventClose(false);
    await windowManager.close();
  }
}

/// Runs the analysis window as a standalone process.
///
/// Launched via `void_player.exe --standalone-analysis --hash=xxx ...`.
/// This gives the analysis window its own D3D11 device and keeps it isolated
/// from the main window renderer.
Future<void> _runStandaloneAnalysis(List<String> args) async {
  final hashes = <String>[];
  final fileNames = <String?>[];
  String? testScriptPath;
  final silentUiTest = _hasFlag(args, '--silent-ui-test');
  int x = 100, y = 100, width = 800, height = 600;
  int accentColorValue = 0xFF0078D4;
  int? analysisIpcPort;
  String? analysisIpcToken;

  for (var i = 0; i < args.length; i++) {
    final arg = args[i];
    if (arg.startsWith('--hash=')) {
      hashes.add(arg.substring(7));
    } else if (arg.startsWith('--fileName=')) {
      final name = arg.substring(11);
      fileNames.add(name.isEmpty ? null : name);
    } else if (arg == '--test-script' && i + 1 < args.length) {
      testScriptPath = args[++i];
    } else if (arg.startsWith('--test-script=')) {
      testScriptPath = arg.substring(14);
    } else if (arg.startsWith('--x=')) {
      x = int.tryParse(arg.substring(4)) ?? 100;
    } else if (arg.startsWith('--y=')) {
      y = int.tryParse(arg.substring(4)) ?? 100;
    } else if (arg.startsWith('--width=')) {
      width = int.tryParse(arg.substring(8)) ?? 800;
    } else if (arg.startsWith('--height=')) {
      height = int.tryParse(arg.substring(9)) ?? 600;
    } else if (arg.startsWith('--accentColor=')) {
      accentColorValue = int.tryParse(arg.substring(14)) ?? 0xFF0078D4;
    } else if (arg.startsWith('--analysis-ipc-port=')) {
      analysisIpcPort = int.tryParse(arg.substring(20));
    } else if (arg.startsWith('--analysis-ipc-token=')) {
      analysisIpcToken = arg.substring(21);
    }
  }

  if (hashes.isEmpty) {
    log.severe('[StandaloneAnalysis] --hash is required');
    exit(1);
  }

  log.info(
    '[StandaloneAnalysis] starting: hashes=$hashes, fileNames=$fileNames, '
    'silentUiTest=$silentUiTest',
  );

  await windowManager.ensureInitialized();
  final initialHwnd = _currentFlutterRunnerHwnd();
  if (initialHwnd != 0) {
    Win32FFI.moveWindow(initialHwnd, x, y, width, height);
    log.info(
      '[StandaloneAnalysis] applied initial rect via Win32: '
      'hwnd=$initialHwnd, x=$x, y=$y, width=$width, height=$height',
    );
  } else {
    await windowManager.setSize(Size(width.toDouble(), height.toDouble()));
    await windowManager.setPosition(Offset(x.toDouble(), y.toDouble()));
    log.warning(
      '[StandaloneAnalysis] initial HWND unavailable, '
      'falling back to window_manager position',
    );
  }
  await windowManager.setMinimumSize(const Size(400, 300));

  await Window.initialize();
  await Window.setEffect(
    effect: WindowEffect.mica,
    color: const Color(0xCC222222),
  );

  final hwnd = initialHwnd != 0 ? initialHwnd : _currentFlutterRunnerHwnd();
  if (hwnd != 0) {
    final title = hashes.length == 1 && fileNames.isNotEmpty
        ? 'Void Player - ${fileNames.first}'
        : 'Void Player - Analysis';
    Win32FFI.setWindowText(hwnd, title);
  }

  final accentColor = Color(accentColorValue);
  final analysisIpcClient = analysisIpcPort != null && analysisIpcToken != null
      ? await AnalysisIpcClient.connect(
          port: analysisIpcPort,
          token: analysisIpcToken,
        )
      : null;

  if (hashes.length == 1 && analysisIpcClient == null) {
    runApp(
      AnalysisApp(
        accentColor: accentColor,
        hash: hashes.first,
        fileName: fileNames.isNotEmpty ? fileNames.first : null,
        testScriptPath: testScriptPath,
      ),
    );
  } else {
    runApp(
      AnalysisWorkspaceApp(
        accentColor: accentColor,
        hashes: hashes,
        fileNames: fileNames,
        testScriptPath: testScriptPath,
        ipcClient: analysisIpcClient,
      ),
    );
  }

  WidgetsBinding.instance.addPostFrameCallback((_) async {
    await _showWindowForMode(silent: silentUiTest);
  });
}

Future<void> runVoidPlayer(List<String> args) async {
  if (args.contains('--standalone-analysis')) {
    await _runStandaloneAnalysis(args);
    return;
  }

  final startupTrace = _StartupTrace();
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
  startupTrace.mark('arguments parsed');

  await AppConfig.initialize();
  WindowManager.silentUiTest = silentUiTest;
  startupTrace.mark('config initialized');

  await windowManager.ensureInitialized();
  await windowManager.setMinimumSize(const Size(520, 360));
  startupTrace.mark('window manager initialized');

  await _applyInitialMainWindowBounds(testWindow: testWindow);
  startupTrace.mark('window bounds applied');

  await Window.initialize();
  await Window.setEffect(
    effect: WindowEffect.mica,
    color: const Color(0xCC222222),
  );
  startupTrace.mark('window effect applied');

  await windowManager.setPreventClose(true);
  final closeHandler = _CloseHandler();
  windowManager.addListener(closeHandler);
  startupTrace.mark('close handler installed');

  final accentColor = _getWindowsAccentColor();
  WindowManager.accentColorValue = accentColor.toARGB32();
  startupTrace.mark('accent color loaded');
  log.info('Application starting (main window), silentUiTest=$silentUiTest');
  runApp(
    VoidPlayerApp(
      accentColor: accentColor,
      testScriptPath: testScriptPath,
      startupOptions: startupOptions,
    ),
  );
  startupTrace.mark('runApp called');

  WidgetsBinding.instance.addPostFrameCallback((_) async {
    await _showWindowForModeWithTrace(
      silent: silentUiTest,
      trace: startupTrace,
    );
  });
}
