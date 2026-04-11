import 'dart:convert';
import 'package:desktop_multi_window/desktop_multi_window.dart';
import 'window_args.dart';

/// Manages secondary window lifecycle.
class WindowManager {
  WindowManager._();

  static final Map<String, String> _windowIds = {};

  static Future<void> _showWindow(String type) async {
    final existing = _windowIds[type];
    if (existing != null) {
      try {
        final ctrl = WindowController.fromWindowId(existing);
        await ctrl.show();
        return;
      } catch (_) {
        // Window was closed by user — stale ID, recreate.
        _windowIds.remove(type);
      }
    }

    final mainCtrl = await WindowController.fromCurrentEngine();
    final ctrl = await WindowController.create(WindowConfiguration(
      arguments: jsonEncode({
        'type': type,
        'mainWindowId': mainCtrl.windowId,
      }),
      hiddenAtLaunch: false,
    ));
    _windowIds[type] = ctrl.windowId;
    await ctrl.show();
  }

  static Future<void> showStatsWindow() => _showWindow(WindowArgs.stats);
  static Future<void> showMemoryWindow() => _showWindow(WindowArgs.memory);
  static Future<void> showSettingsWindow() => _showWindow(WindowArgs.settings);
}
