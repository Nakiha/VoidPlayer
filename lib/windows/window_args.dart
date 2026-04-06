import 'dart:convert';

/// Window type constants and argument parsing for multi-window routing.
class WindowArgs {
  final String windowType;

  const WindowArgs._({required this.windowType});

  /// Window type constants
  static const String main = 'main';
  static const String stats = 'stats';
  static const String memory = 'memory';
  static const String settings = 'settings';

  /// Parse CLI args to determine which window type this is.
  ///
  /// Main window (engine #0) receives no desktop_multi_window args.
  /// Secondary windows receive their type as JSON via createWindow arguments.
  static WindowArgs parse(List<String> args) {
    // Look for multi-window argument pattern.
    // desktop_multi_window passes args like: --multi-window <windowId> <json>
    for (int i = 0; i < args.length; i++) {
      final arg = args[i];
      if (arg == '--multi-window' && i + 2 < args.length) {
        // The JSON argument follows the window ID
        try {
          final config = jsonDecode(args[i + 2]) as Map<String, dynamic>;
          final type = config['type'] as String? ?? WindowArgs.main;
          return WindowArgs._(windowType: type);
        } catch (_) {
          // Not valid JSON, continue looking
 }
      }
    }
    // Default: main window
    return WindowArgs._(windowType: WindowArgs.main);
  }
}
