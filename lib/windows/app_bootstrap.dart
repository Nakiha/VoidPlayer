import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_acrylic/flutter_acrylic.dart';
import 'package:window_manager/window_manager.dart';

import '../app.dart';
import '../app_log.dart';
import '../config/app_config.dart';
import '../platform/main_window_shutdown.dart';
import '../platform/native_file_picker.dart';
import '../platform/platform_capabilities.dart';
import '../platform/window_bootstrap_args.dart';
import '../platform/window_bounds_policy.dart';
import '../startup_options.dart';
import 'system_accent_watcher.dart';
import 'win32_pointer_button_state_provider.dart';
import 'win32ffi.dart';
import 'windows_main_window_platform.dart';

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

Color _getWindowsAccentColor() => Color(Win32FFI.getDwmAccentColorArgb());

bool _isRestorableMainWindowRect(Rect? rect) {
  return isRestorableWindowRect(rect, isOnScreen: Win32FFI.isRectOnScreen);
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

/// Handles the close button, saves window state, then closes the main window.
class _CloseHandler with WindowListener {
  @override
  void onWindowClose() async {
    windowManager.removeListener(this);
    try {
      final bounds = await windowManager.getBounds();
      AppConfig.instance.windowRect = bounds;
      await AppConfig.instance.save();
    } catch (error, stack) {
      log.warning('[Startup] window state save failed', error, stack);
    }
    try {
      await windowManager.hide();
      log.info('[Startup] main window hidden before shutdown');
    } catch (error, stack) {
      log.warning(
        '[Startup] main window hide before shutdown failed',
        error,
        stack,
      );
    }
    try {
      await MainWindowShutdownRegistry.closeGracefully();
    } catch (error, stack) {
      log.severe('[Startup] main window shutdown failed', error, stack);
    }
    await windowManager.setPreventClose(false);
    await windowManager.close();
  }
}

Future<void> runVoidPlayer(List<String> args) async {
  final startupTrace = _StartupTrace();
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
  startupTrace.mark('arguments parsed');

  await AppConfig.initialize();
  startupTrace.mark('config initialized');

  await windowManager.ensureInitialized();
  await windowManager.setMinimumSize(kMinimumMainWindowSize);
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
  startupTrace.mark('accent color loaded');
  log.info('Application starting (main window), silentUiTest=$silentUiTest');
  runApp(
    VoidPlayerApp(
      accentColor: accentColor,
      platformCapabilities: PlatformCapabilities.windows,
      systemAccentWatcherFactory: WindowsSystemAccentWatcher.new,
      platformWindow: const WindowsMainWindowPlatform(),
      nativeFilePicker: const MethodChannelNativeFilePicker(),
      pointerButtonStateProvider: const Win32PointerButtonStateProvider(),
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
