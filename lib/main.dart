import 'dart:io';
import 'dart:ui';
import 'package:flutter/material.dart';
import 'package:flutter_acrylic/flutter_acrylic.dart';
import 'package:window_manager/window_manager.dart' hide WindowManager;
import 'l10n/app_localizations.dart';
import 'app_log.dart';
import 'config/app_config.dart';
import 'actions/action_registry.dart';
import 'windows/win32ffi.dart';
import 'windows/window_args.dart';
import 'windows/window_manager.dart';
import 'windows/main_window.dart';
import 'windows/stats_window.dart';
import 'windows/memory_window.dart';
import 'windows/settings_window.dart';
import 'windows/analysis_window.dart';

Future<Color> getWindowsAccentColor() async {
  try {
    final result = await Process.run('powershell', [
      '-Command',
      "(Get-ItemProperty -Path 'HKCU:\\Software\\Microsoft\\Windows\\DWM' -Name 'AccentColor').AccentColor",
    ]);
    final value = int.parse(result.stdout.trim());
    final r = value & 0xFF;
    final g = (value >> 8) & 0xFF;
    final b = (value >> 16) & 0xFF;
    return Color.fromARGB(255, r, g, b);
  } catch (_) {
    return const Color(0xFF0078D4);
  }
}

/// Checks whether [rect] overlaps with any connected display.
bool _isRectOnScreen(Rect rect) {
  for (final display in PlatformDispatcher.instance.displays) {
    final w = display.size.width / display.devicePixelRatio;
    final h = display.size.height / display.devicePixelRatio;
    if (rect.overlaps(Rect.fromLTWH(0, 0, w, h))) return true;
  }
  return false;
}

/// Applies the initial position/size for a secondary window via FFI.
///
/// Must be called **before** `runApp()` so that Flutter's rendering surface
/// is initialized at the correct size from the very first frame.
void _applySecondaryWindowRect(WindowArgs windowArgs) {
  final rect = windowArgs.initialRect;
  if (rect == null) return;
  final hwnd = Win32FFI.getForegroundWindow();
  if (hwnd != 0) {
    Win32FFI.moveWindow(
      hwnd,
      rect.left.toInt(),
      rect.top.toInt(),
      rect.width.toInt(),
      rect.height.toInt(),
    );
  }
}

/// Handles the close button: saves all window state, closes secondary
/// windows, then closes the main window.
class _CloseHandler with WindowListener {
  @override
  void onWindowClose() async {
    windowManager.removeListener(this);
    // Save main window rect.
    final bounds = await windowManager.getBounds();
    AppConfig.instance.windowRect = bounds;
    await AppConfig.instance.save();
    // Close all secondary windows *before* closing the main window to
    // prevent native crash (stack overflow in naki_analysis_unload).
    await WindowManager.closeAllSecondaryWindows();
    await windowManager.setPreventClose(false);
    await windowManager.close();
  }
}

void main(List<String> args) async {
  WidgetsFlutterBinding.ensureInitialized();
  await initLogging(args);

  // Extract --test-script path (if any) before window routing.
  String? testScriptPath;
  final scriptIdx = args.indexOf('--test-script');
  if (scriptIdx >= 0 && scriptIdx + 1 < args.length) {
    testScriptPath = args[scriptIdx + 1];
  }

  // Determine window type from arguments.
  final windowArgs = WindowArgs.parse(args);

  switch (windowArgs.windowType) {
    case WindowArgs.stats:
    case WindowArgs.memory:
    case WindowArgs.settings:
    case WindowArgs.analysis:
      log.info('[SecondaryWindow] type=${windowArgs.windowType}, initializing...');
      await Window.initialize();
      await Window.setEffect(
        effect: WindowEffect.mica,
        color: const Color(0xCC222222),
      );
      final accentColor = windowArgs.accentColor;
      final app = switch (windowArgs.windowType) {
        WindowArgs.stats => StatsApp(accentColor: accentColor),
        WindowArgs.memory => MemoryApp(accentColor: accentColor),
        WindowArgs.settings => SettingsApp(accentColor: accentColor),
        WindowArgs.analysis => AnalysisApp(accentColor: accentColor, hash: windowArgs.hash!, fileName: windowArgs.fileName),
        _ => throw StateError('unreachable'),
      };
      // Apply initial position/size before the first frame renders.
      _applySecondaryWindowRect(windowArgs);
      // Set native window title (desktop_multi_window doesn't provide this).
      final hwnd = Win32FFI.getForegroundWindow();
      if (hwnd != 0) {
        final title = switch (windowArgs.windowType) {
          WindowArgs.analysis => windowArgs.fileName != null
              ? 'Void Player - ${windowArgs.fileName}'
              : 'Void Player - Analysis',
          _ => WindowArgs.windowTitles[windowArgs.windowType] ?? 'Void Player',
        };
        Win32FFI.setWindowText(hwnd, title);
      }
      runApp(app);
      break;
    default:
      await AppConfig.initialize();

      // window_manager needs ensureInitialized() to capture the native HWND.
      // Without it, all subsequent calls (setSize, center, show…) operate on
      // an invalid handle and silently fail or hang.
      await windowManager.ensureInitialized();
      await windowManager.setMinimumSize(const Size(520, 360));

      final savedRect = AppConfig.instance.windowRect;
      if (savedRect != null && _isRectOnScreen(savedRect)) {
        await windowManager.setSize(savedRect.size);
        await windowManager.setPosition(savedRect.topLeft);
      } else {
        await windowManager.setSize(const Size(1280, 720));
        await windowManager.center();
      }

      await Window.initialize();
      await Window.setEffect(
        effect: WindowEffect.mica,
        color: const Color(0xCC222222),
      );

      await windowManager.setPreventClose(true);
      final closeHandler = _CloseHandler();
      windowManager.addListener(closeHandler);

      final accentColor = await getWindowsAccentColor();
      WindowManager.accentColorValue = accentColor.toARGB32();
      log.info('Application starting (main window)');
      runApp(MyApp(accentColor: accentColor, testScriptPath: testScriptPath));

      // Show window after first frame renders to prevent white flash on slow PCs.
      WidgetsBinding.instance.addPostFrameCallback((_) async {
        await windowManager.show();
      });
  }
}

class MyApp extends StatelessWidget {
  final Color accentColor;
  final String? testScriptPath;
  const MyApp({super.key, required this.accentColor, this.testScriptPath});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player',
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: accentColor,
          brightness: Brightness.light,
        ),
      ),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: accentColor,
          brightness: Brightness.dark,
        ),
      ),
      themeMode: ThemeMode.system,
      home: ActionFocus(
        child: MainWindow(testScriptPath: testScriptPath),
      ),
    );
  }
}
