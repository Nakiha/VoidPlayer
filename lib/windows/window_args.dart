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
  /// Secondary windows receive args: ["multi_window", "<windowId>", "<json>"]
  static WindowArgs parse(List<String> args) {
    for (int i = 0; i < args.length; i++) {
      final arg = args[i];
      if (arg == 'multi_window' && i + 2 < args.length) {
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
