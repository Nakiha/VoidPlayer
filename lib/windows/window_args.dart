import 'dart:convert';
import 'dart:ui';

/// Window type constants and argument parsing for multi-window routing.
class WindowArgs {
  final String windowType;
  final int? _accentColorValue;
  final String? hash;

  const WindowArgs._({required this.windowType, int? accentColorValue, this.hash})
      : _accentColorValue = accentColorValue;

  /// Window type constants
  static const String main = 'main';
  static const String stats = 'stats';
  static const String memory = 'memory';
  static const String settings = 'settings';
  static const String analysis = 'analysis';

  /// The accent color passed from the main window, or fallback.
  Color get accentColor =>
      _accentColorValue != null ? Color(_accentColorValue) : const Color(0xFF0078D4);

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
          final accentColor = config['accentColor'] as int?;
          final hash = config['hash'] as String?;
          return WindowArgs._(windowType: type, accentColorValue: accentColor, hash: hash);
        } catch (_) {
          // Not valid JSON, continue looking
        }
      }
    }
    // Default: main window
    return const WindowArgs._(windowType: WindowArgs.main);
  }
}
