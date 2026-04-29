import 'dart:convert';
import 'dart:ui';

/// Window type constants and argument parsing for multi-window routing.
class WindowArgs {
  final String windowType;
  final int? _accentColorValue;
  final String? hash;
  final String? fileName;
  final Rect? _initialRect;

  const WindowArgs._({
    required this.windowType,
    int? accentColorValue,
    this.hash,
    this.fileName,
    Rect? initialRect,
  }) : _accentColorValue = accentColorValue,
       _initialRect = initialRect;

  /// Window type constants
  static const String main = 'main';
  static const String settings = 'settings';
  static const String analysis = 'analysis';

  /// The accent color passed from the main window, or fallback.
  Color get accentColor => _accentColorValue != null
      ? Color(_accentColorValue)
      : const Color(0xFF0078D4);

  /// The initial position/size passed from the main window, or `null`.
  Rect? get initialRect => _initialRect;

  /// Window titles for identifying secondary windows.
  static const Map<String, String> windowTitles = {
    settings: 'Void Player - Settings',
    analysis: 'Void Player - Analysis',
  };

  /// Default sizes for each window type.
  static const Map<String, (int, int)> defaultSizes = {
    settings: (700, 500),
    analysis: (1000, 700),
  };

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
          final fileName = config['fileName'] as String?;

          // Parse initial position/size if present.
          Rect? initialRect;
          final x = config['x'];
          final y = config['y'];
          final w = config['width'];
          final h = config['height'];
          if (x is num && y is num && w is num && h is num) {
            initialRect = Rect.fromLTWH(
              x.toDouble(),
              y.toDouble(),
              w.toDouble(),
              h.toDouble(),
            );
          }

          return WindowArgs._(
            windowType: type,
            accentColorValue: accentColor,
            hash: hash,
            fileName: fileName,
            initialRect: initialRect,
          );
        } catch (_) {
          // Not valid JSON, continue looking
        }
      }
    }
    // Default: main window
    return const WindowArgs._(windowType: WindowArgs.main);
  }
}
