import 'dart:convert';
import 'dart:ui';
import 'package:desktop_multi_window/desktop_multi_window.dart';
import '../app_log.dart';
import '../config/app_config.dart';
import 'win32ffi.dart';
import 'window_args.dart';

/// Manages secondary window lifecycle: creation, positioning, and shutdown.
class WindowManager {
  WindowManager._();

  static final Map<String, String> _windowIds = {};

  /// Accent color set by the main window, passed to all secondary windows.
  static int accentColorValue = 0xFF0078D4;

  // -----------------------------------------------------------------------
  // Public API
  // -----------------------------------------------------------------------

  static Future<void> showStatsWindow() => _showWindow(WindowArgs.stats);
  static Future<void> showMemoryWindow() => _showWindow(WindowArgs.memory);
  static Future<void> showSettingsWindow() => _showWindow(WindowArgs.settings);

  /// Show an analysis window for a specific video hash.
  /// Each hash opens a separate window.
  static Future<void> showAnalysisWindow(String hash, {String? fileName}) =>
      _showKeyedWindow(WindowArgs.analysis, hash,
          extraConfig: {'hash': hash, if (fileName != null) 'fileName': fileName});

  /// Saves all secondary window positions to config, then closes them.
  ///
  /// Must be called from the main window's close handler *before* the main
  /// window itself is closed.
  static Future<void> closeAllSecondaryWindows() async {
    // 1. Save positions of all currently-open secondary windows.
    await _saveAllPositions();

    // 2. Find all secondary windows by class name and send WM_CLOSE.
    final hwnds = Win32FFI.findSecondaryWindows();
    if (hwnds.isEmpty) return;

    for (final hwnd in hwnds) {
      Win32FFI.forceClose(hwnd);
    }

    // 3. Wait for windows to actually close (up to ~2 s).
    for (int i = 0; i < 20; i++) {
      await Future.delayed(const Duration(milliseconds: 100));
      if (hwnds.every((h) => !Win32FFI.isWindow(h))) break;
    }

    _windowIds.clear();
  }

  // -----------------------------------------------------------------------
  // Internals
  // -----------------------------------------------------------------------

  /// Check whether a secondary window of [type] is still alive by title.
  static bool _isSecondaryWindowAlive(String type) {
    final title = WindowArgs.windowTitles[type];
    if (title == null) return false;
    for (final hwnd in Win32FFI.findSecondaryWindows()) {
      if (Win32FFI.getWindowText(hwnd).contains(title)) return true;
    }
    return false;
  }

  static Future<void> _showWindow(String type) async {
    // Check for an existing window of this type.
    final existing = _windowIds[type];
    if (existing != null) {
      if (!_isSecondaryWindowAlive(type)) {
        log.info('[WindowManager] "$type" closed by user, removing stale id=$existing');
        _windowIds.remove(type);
      } else {
        try {
          final ctrl = WindowController.fromWindowId(existing);
          await ctrl.show();
          return;
        } catch (e, stack) {
          log.warning('[WindowManager] "$type" show() failed for id=$existing: $e\n$stack');
          _windowIds.remove(type);
        }
      }
    }

    // Compute the initial rect (from saved config or cascade from parent).
    final rect = await _computeWindowRect(type);

    final mainCtrl = await WindowController.fromCurrentEngine();
    log.info('[WindowManager] creating "$type" window, rect=$rect, mainWindowId=${mainCtrl.windowId}');
    final ctrl = await WindowController.create(WindowConfiguration(
      arguments: jsonEncode({
        'type': type,
        'mainWindowId': mainCtrl.windowId,
        'accentColor': accentColorValue,
        'x': rect.left.toInt(),
        'y': rect.top.toInt(),
        'width': rect.width.toInt(),
        'height': rect.height.toInt(),
      }),
      hiddenAtLaunch: true,
    ));
    _windowIds[type] = ctrl.windowId;
    log.info('[WindowManager] "$type" created with id=${ctrl.windowId}');
  }

  /// Show a keyed window — each unique [key] gets its own window.
  static Future<void> _showKeyedWindow(String type, String key,
      {Map<String, dynamic>? extraConfig}) async {
    final fullKey = '${type}_$key';
    final existing = _windowIds[fullKey];
    if (existing != null) {
      try {
        log.info('[WindowManager] "$fullKey" already exists (id=$existing), calling show()');
        final ctrl = WindowController.fromWindowId(existing);
        await ctrl.show();
        return;
      } catch (e, stack) {
        log.warning('[WindowManager] "$fullKey" show() failed for id=$existing: $e\n$stack');
        _windowIds.remove(fullKey);
      }
    }

    // Compute the initial rect for keyed windows too.
    final rect = await _computeWindowRect(type);

    final mainCtrl = await WindowController.fromCurrentEngine();
    log.info('[WindowManager] creating "$fullKey" window, rect=$rect, mainWindowId=${mainCtrl.windowId}');
    final config = <String, dynamic>{
      'type': type,
      'mainWindowId': mainCtrl.windowId,
      'accentColor': accentColorValue,
      'x': rect.left.toInt(),
      'y': rect.top.toInt(),
      'width': rect.width.toInt(),
      'height': rect.height.toInt(),
    };
    if (extraConfig != null) config.addAll(extraConfig);

    final ctrl = await WindowController.create(WindowConfiguration(
      arguments: jsonEncode(config),
      hiddenAtLaunch: true,
    ));
    _windowIds[fullKey] = ctrl.windowId;
    log.info('[WindowManager] "$fullKey" created with id=${ctrl.windowId}');
  }

  /// Computes the initial position and size for a secondary window:
  /// 1. Try the saved rect from config (if still on-screen).
  /// 2. Otherwise cascade from the main (parent) window.
  static Future<Rect> _computeWindowRect(String type) async {
    final (defaultW, defaultH) =
        WindowArgs.defaultSizes[type] ?? (800, 600);

    // Try saved position.
    final saved = AppConfig.instance.secondaryWindowRect(type);
    if (saved != null && Win32FFI.isRectOnScreen(saved)) {
      return saved;
    }

    // Cascade from the main window.
    final parentHwnd = Win32FFI.findWindow(
      className: kMainWindowClass,
    );
    if (parentHwnd != 0) {
      final parentRect = Win32FFI.getWindowRect(parentHwnd);
      final monitorArea = Win32FFI.getMonitorWorkArea(parentHwnd);
      return Win32FFI.cascadePosition(
        parentRect, monitorArea,
        defaultWidth: defaultW,
        defaultHeight: defaultH,
      );
    }

    // Fallback.
    return Rect.fromLTWH(100, 100, defaultW.toDouble(), defaultH.toDouble());
  }

  /// Queries all open secondary windows and saves their rects to config.
  static Future<void> _saveAllPositions() async {
    final hwnds = Win32FFI.findSecondaryWindows();
    for (final hwnd in hwnds) {
      final title = Win32FFI.getWindowText(hwnd);
      for (final entry in WindowArgs.windowTitles.entries) {
        if (title.contains(entry.value)) {
          final rect = Win32FFI.getWindowRect(hwnd);
          AppConfig.instance.setSecondaryWindowRect(entry.key, rect);
          break;
        }
      }
    }
    await AppConfig.instance.save();
  }
}
