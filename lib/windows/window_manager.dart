import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui';
import 'package:desktop_multi_window/desktop_multi_window.dart';
import '../app_log.dart';
import '../config/app_config.dart';
import 'win32ffi.dart';
import 'window_args.dart';

typedef AnalysisWindowRequest = ({String hash, String? fileName});

/// Manages secondary window lifecycle: creation, positioning, and shutdown.
class WindowManager {
  WindowManager._();

  static final Map<String, String> _windowIds = {};

  /// Analysis processes spawned as separate processes (keyed by hash).
  static final Map<String, Process> _analysisProcesses = {};
  static final Map<String, int> _analysisExitCodes = {};
  static String? analysisTestScriptPath;
  static bool silentUiTest = false;
  static int? analysisIpcPort;
  static String? analysisIpcToken;

  /// Accent color set by the main window, passed to all secondary windows.
  static int accentColorValue = 0xFF0078D4;

  // -----------------------------------------------------------------------
  // Public API
  // -----------------------------------------------------------------------

  static Future<void> showSettingsWindow() => _showWindow(WindowArgs.settings);

  /// Show an analysis window for a specific video hash.
  /// Each hash opens a separate process to avoid Flutter's D3D11 multi-engine crash.
  static Future<void> showAnalysisWindow(String hash, {String? fileName}) =>
      _spawnAnalysisProcess(hash, fileName: fileName);

  /// Show multiple analysis windows from one user action, arranged as a batch.
  static Future<void> showAnalysisWindows(
    List<AnalysisWindowRequest> windows,
  ) async {
    if (windows.isEmpty) return;
    if (windows.length == 1) {
      final window = windows.first;
      await showAnalysisWindow(window.hash, fileName: window.fileName);
      return;
    }

    await _spawnAnalysisWorkspaceProcess(windows);
  }

  static int get analysisProcessCount => _analysisProcesses.length;

  static Map<String, int> get analysisExitCodes =>
      Map.unmodifiable(_analysisExitCodes);

  static Future<bool> waitForAnalysisProcessCount(
    int count,
    Duration timeout,
  ) async {
    final sw = Stopwatch()..start();
    while (sw.elapsed < timeout) {
      if (_analysisProcesses.length == count) return true;
      await Future.delayed(const Duration(milliseconds: 100));
    }
    return _analysisProcesses.length == count;
  }

  /// Saves all secondary window positions to config, then closes them.
  ///
  /// Must be called from the main window's close handler *before* the main
  /// window itself is closed.
  static Future<void> closeAllSecondaryWindows() async {
    // 1. Save positions of all currently-open secondary windows.
    await _saveAllPositions();

    // 2. Find all secondary windows by class name and send WM_CLOSE.
    final hwnds = Win32FFI.findSecondaryWindows();
    for (final hwnd in hwnds) {
      Win32FFI.forceClose(hwnd);
    }

    // 3. Wait for secondary windows to actually close (up to ~2 s).
    for (int i = 0; i < 20; i++) {
      await Future.delayed(const Duration(milliseconds: 100));
      if (hwnds.every((h) => !Win32FFI.isWindow(h))) break;
    }

    _windowIds.clear();

    // 4. Kill any spawned analysis processes.
    for (final process in _analysisProcesses.values) {
      process.kill();
    }
    _analysisProcesses.clear();
    _analysisExitCodes.clear();
    analysisIpcPort = null;
    analysisIpcToken = null;
  }

  /// Compute the initial rect for a secondary window of the given [type].
  /// Public so that main/main_window.dart can use it for process-spawned windows.
  static Future<Rect> computeWindowRect(String type) =>
      _computeWindowRect(type);

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

  // --- Analysis process spawning ---

  static Future<void> _spawnAnalysisProcess(
    String hash, {
    String? fileName,
    Rect? initialRect,
  }) async {
    final key = _analysisProcessKey(hash);
    // If a process for this hash/workspace is already running, don't spawn another.
    if (_analysisProcesses.containsKey(key)) {
      log.info(
        '[WindowManager] analysis process for $key still running, skipping',
      );
      return;
    }

    final rect = initialRect ?? await _computeAnalysisWindowRect();
    final exe = Platform.resolvedExecutable;
    final scriptPath = analysisTestScriptPath;
    _analysisExitCodes.remove(key);

    final args = <String>[
      '--standalone-analysis',
      '--hash=$hash',
      '--x=${rect.left.toInt()}',
      '--y=${rect.top.toInt()}',
      '--width=${rect.width.toInt()}',
      '--height=${rect.height.toInt()}',
      '--accentColor=$accentColorValue',
      if (fileName != null) '--fileName=$fileName',
      if (scriptPath != null) ...['--test-script', scriptPath],
      if (analysisIpcPort != null) '--analysis-ipc-port=$analysisIpcPort',
      if (analysisIpcToken != null) '--analysis-ipc-token=$analysisIpcToken',
      if (silentUiTest) '--silent-ui-test',
    ];

    log.info('[WindowManager] spawning analysis process: $args');
    final process = await Process.start(exe, args);
    _analysisProcesses[key] = process;

    // Log stderr for debugging.
    process.stderr.transform(utf8.decoder).listen((data) {
      log.warning('[AnalysisProcess:$key] stderr: $data');
    });

    // Clean up when process exits.
    process.exitCode.then((code) {
      log.info(
        '[WindowManager] analysis process for $key exited with code $code',
      );
      _analysisProcesses.remove(key);
      _analysisExitCodes[key] = code;
    });
  }

  static Future<void> _spawnAnalysisWorkspaceProcess(
    List<AnalysisWindowRequest> windows,
  ) async {
    final key = _analysisProcessKey(
      'workspace:${windows.map((w) => w.hash).join('|')}',
    );
    if (_analysisProcesses.containsKey(key)) {
      log.info(
        '[WindowManager] analysis workspace for ${windows.length} tracks still running, skipping',
      );
      return;
    }

    final rect = await _computeAnalysisWindowRect();
    final exe = Platform.resolvedExecutable;
    final scriptPath = analysisTestScriptPath;
    _analysisExitCodes.remove(key);

    final args = <String>[
      '--standalone-analysis',
      for (final window in windows) '--hash=${window.hash}',
      '--x=${rect.left.toInt()}',
      '--y=${rect.top.toInt()}',
      '--width=${rect.width.toInt()}',
      '--height=${rect.height.toInt()}',
      '--accentColor=$accentColorValue',
      for (final window in windows) '--fileName=${window.fileName ?? ''}',
      if (scriptPath != null) ...['--test-script', scriptPath],
      if (analysisIpcPort != null) '--analysis-ipc-port=$analysisIpcPort',
      if (analysisIpcToken != null) '--analysis-ipc-token=$analysisIpcToken',
      if (silentUiTest) '--silent-ui-test',
    ];

    log.info('[WindowManager] spawning analysis workspace process: $args');
    final process = await Process.start(exe, args);
    _analysisProcesses[key] = process;

    process.stderr.transform(utf8.decoder).listen((data) {
      log.warning('[AnalysisWorkspaceProcess] stderr: $data');
    });

    process.exitCode.then((code) {
      log.info('[WindowManager] analysis workspace exited with code $code');
      _analysisProcesses.remove(key);
      _analysisExitCodes[key] = code;
    });
  }

  static String _analysisProcessKey(String fallback) =>
      analysisIpcPort != null ? 'workspace:ipc' : fallback;

  static Future<Rect> _computeAnalysisWindowRect() =>
      _computeWindowRect(WindowArgs.analysis, useSavedPosition: false);

  static int _findMainWindowHwnd() {
    final foregroundHwnd = Win32FFI.getForegroundWindow();
    if (Win32FFI.isCurrentProcessWindowOfClass(
      foregroundHwnd,
      kMainWindowClass,
    )) {
      return foregroundHwnd;
    }

    final hwnds = Win32FFI.findCurrentProcessWindowsByClass(kMainWindowClass);
    for (final hwnd in hwnds) {
      if (Win32FFI.getWindowText(hwnd) == 'Void Player') {
        return hwnd;
      }
    }
    return hwnds.isNotEmpty ? hwnds.first : 0;
  }

  // --- desktop_multi_window secondary windows (settings only) ---

  static Future<void> _showWindow(String type) async {
    // Check for an existing window of this type.
    final existing = _windowIds[type];
    if (existing != null) {
      if (!_isSecondaryWindowAlive(type)) {
        log.info(
          '[WindowManager] "$type" closed by user, removing stale id=$existing',
        );
        _windowIds.remove(type);
      } else {
        try {
          final ctrl = WindowController.fromWindowId(existing);
          await ctrl.show();
          return;
        } catch (e, stack) {
          log.warning(
            '[WindowManager] "$type" show() failed for id=$existing: $e\n$stack',
          );
          _windowIds.remove(type);
        }
      }
    }

    // Compute the initial rect (from saved config or cascade from parent).
    final rect = await _computeWindowRect(type);

    final mainCtrl = await WindowController.fromCurrentEngine();
    log.info(
      '[WindowManager] creating "$type" window, rect=$rect, mainWindowId=${mainCtrl.windowId}',
    );
    try {
      final ctrl = await WindowController.create(
        WindowConfiguration(
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
        ),
      );
      _windowIds[type] = ctrl.windowId;
      log.info('[WindowManager] "$type" created with id=${ctrl.windowId}');
    } finally {
      // Small delay for the engine to stabilize.
      await Future.delayed(const Duration(milliseconds: 100));
    }
  }

  /// Computes the initial position and size for a secondary window:
  /// 1. Try the saved rect from config (if still on-screen).
  /// 2. Otherwise cascade from the main (parent) window.
  static Future<Rect> _computeWindowRect(
    String type, {
    bool useSavedPosition = true,
  }) async {
    final (defaultW, defaultH) = WindowArgs.defaultSizes[type] ?? (800, 600);

    // Try saved position.
    if (useSavedPosition) {
      final saved = AppConfig.instance.secondaryWindowRect(type);
      if (saved != null && Win32FFI.isRectOnScreen(saved)) {
        return saved;
      }
    }

    // Cascade from the main window.
    final parentHwnd = _findMainWindowHwnd();
    if (parentHwnd != 0) {
      final parentRect = Win32FFI.getWindowRect(parentHwnd);
      final monitorArea = Win32FFI.getMonitorWorkArea(parentHwnd);
      final offset = type == WindowArgs.analysis
          ? Win32FFI.titleBarOffset()
          : 32;
      if (type == WindowArgs.analysis) {
        log.info(
          '[WindowManager] analysis anchor hwnd=$parentHwnd, '
          'title="${Win32FFI.getWindowText(parentHwnd)}", '
          'rect=(${parentRect.left.toInt()}, ${parentRect.top.toInt()}, '
          '${parentRect.width.toInt()}x${parentRect.height.toInt()}), '
          'offset=$offset',
        );
      }
      return Win32FFI.cascadePosition(
        parentRect,
        monitorArea,
        offset: offset,
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
