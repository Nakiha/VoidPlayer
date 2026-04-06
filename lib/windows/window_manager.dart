import 'dart:convert';
import 'package:desktop_multi_window/desktop_multi_window.dart';
import 'window_args.dart';

/// Manages secondary window lifecycle.
class WindowManager {
  WindowManager._();

  static final Map<String, String> _windowIds = {};

  /// Show or focus an existing stats window, or create a new one.
  static Future<void> showStatsWindow() async {
    final existing = _windowIds[WindowArgs.stats];
    if (existing != null) {
      final ctrl = WindowController.fromWindowId(existing);
      await ctrl.show();
      return;
    }

    final ctrl = await WindowController.create(WindowConfiguration(
      arguments: jsonEncode({'type': WindowArgs.stats}),
      hiddenAtLaunch: false,
    ));
    _windowIds[WindowArgs.stats] = ctrl.windowId;
    await ctrl.show();
  }

  /// Show or focus an existing memory window.
  static Future<void> showMemoryWindow() async {
    final existing = _windowIds[WindowArgs.memory];
    if (existing != null) {
      final ctrl = WindowController.fromWindowId(existing);
      await ctrl.show();
      return;
    }

    final ctrl = await WindowController.create(WindowConfiguration(
      arguments: jsonEncode({'type': WindowArgs.memory}),
      hiddenAtLaunch: false,
    ),
    );
    _windowIds[WindowArgs.memory] = ctrl.windowId;
    await ctrl.show();
  }

  /// Show or focus an existing settings window.
  static Future<void> showSettingsWindow() async {
    final existing = _windowIds[WindowArgs.settings];
    if (existing != null) {
      final ctrl = WindowController.fromWindowId(existing);
      await ctrl.show();
      return;
    }

    final ctrl = await WindowController.create(WindowConfiguration(
      arguments: jsonEncode({'type': WindowArgs.settings}),
      hiddenAtLaunch: false,
    ),
    );
    _windowIds[WindowArgs.settings] = ctrl.windowId;
    await ctrl.show();
  }
}
