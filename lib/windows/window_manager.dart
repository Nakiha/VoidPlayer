import 'dart:convert';
import 'package:desktop_multi_window/desktop_multi_window.dart';
import 'window_args.dart';

/// Manages secondary window lifecycle.
class WindowManager {
  WindowManager._();

  static final Map<String, String> _windowIds = {};

  /// Accent color set by the main window, passed to all secondary windows.
  static int accentColorValue = 0xFF0078D4;

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
        'accentColor': accentColorValue,
      }),
      hiddenAtLaunch: false,
    ));
    _windowIds[type] = ctrl.windowId;
    await ctrl.show();
  }

  /// Show a keyed window — each unique [key] gets its own window.
  static Future<void> _showKeyedWindow(String type, String key,
      {Map<String, dynamic>? extraConfig}) async {
    final fullKey = '${type}_$key';
    final existing = _windowIds[fullKey];
    if (existing != null) {
      try {
        final ctrl = WindowController.fromWindowId(existing);
        await ctrl.show();
        return;
      } catch (_) {
        _windowIds.remove(fullKey);
      }
    }

    final mainCtrl = await WindowController.fromCurrentEngine();
    final config = <String, dynamic>{
      'type': type,
      'mainWindowId': mainCtrl.windowId,
      'accentColor': accentColorValue,
    };
    if (extraConfig != null) config.addAll(extraConfig);

    final ctrl = await WindowController.create(WindowConfiguration(
      arguments: jsonEncode(config),
      hiddenAtLaunch: false,
    ));
    _windowIds[fullKey] = ctrl.windowId;
    await ctrl.show();
  }

  static Future<void> showStatsWindow() => _showWindow(WindowArgs.stats);
  static Future<void> showMemoryWindow() => _showWindow(WindowArgs.memory);
  static Future<void> showSettingsWindow() => _showWindow(WindowArgs.settings);

  /// Show an analysis window for a specific video hash.
  /// Each hash opens a separate window.
  static Future<void> showAnalysisWindow(String hash) =>
      _showKeyedWindow(WindowArgs.analysis, hash, extraConfig: {'hash': hash});
}
