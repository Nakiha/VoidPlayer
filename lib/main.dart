import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_acrylic/flutter_acrylic.dart';
import 'app_log.dart';
import 'actions/action_registry.dart';
import 'windows/window_args.dart';
import 'windows/main_window.dart';
import 'windows/stats_window.dart';
import 'windows/memory_window.dart';
import 'windows/settings_window.dart';

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
      runApp(const StatsApp());
      break;
    case WindowArgs.memory:
      runApp(const MemoryApp());
      break;
    case WindowArgs.settings:
      runApp(const SettingsApp());
      break;
    default:
      // Main window: apply Mica effect and launch player.
      await Window.initialize();
      await Window.setEffect(
        effect: WindowEffect.mica,
        color: const Color(0xCC222222),
      );
      final accentColor = await getWindowsAccentColor();
      log.info('Application starting (main window)');
      runApp(MyApp(accentColor: accentColor, testScriptPath: testScriptPath));
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
