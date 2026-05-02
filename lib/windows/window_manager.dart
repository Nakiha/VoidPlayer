import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui';

import '../app_log.dart';
import 'win32ffi.dart';

typedef AnalysisWindowRequest = ({String hash, String? fileName});

const String _analysisWindowType = 'analysis';
const (int, int) _analysisDefaultSize = (1000, 700);

/// Manages external analysis processes and their lifecycle.
class WindowManager {
  WindowManager._();

  /// Analysis processes spawned as separate processes (keyed by hash).
  static final Map<String, Process> _analysisProcesses = {};
  static final Map<String, int> _analysisExitCodes = {};
  static String? analysisTestScriptPath;
  static bool silentUiTest = false;
  static int? analysisIpcPort;
  static String? analysisIpcToken;

  /// Accent color set by the main window, passed to analysis processes.
  static int accentColorValue = 0xFF0078D4;

  /// Show an analysis window for a specific video hash.
  static Future<void> showAnalysisWindow(
    String hash, {
    String? fileName,
    void Function()? onExit,
  }) => _spawnAnalysisProcess(hash, fileName: fileName, onExit: onExit);

  /// Show multiple analysis views from one user action, arranged as a batch.
  static Future<void> showAnalysisWindows(
    List<AnalysisWindowRequest> windows, {
    void Function()? onExit,
  }) async {
    if (windows.isEmpty) return;
    if (windows.length == 1) {
      final window = windows.first;
      await showAnalysisWindow(
        window.hash,
        fileName: window.fileName,
        onExit: onExit,
      );
      return;
    }

    await _spawnAnalysisWorkspaceProcess(windows, onExit: onExit);
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

  /// Closes all analysis child processes before the main window exits.
  static Future<void> closeAllAnalysisWindows() async {
    for (final process in _analysisProcesses.values) {
      process.kill();
    }
    _analysisProcesses.clear();
    _analysisExitCodes.clear();
    analysisIpcPort = null;
    analysisIpcToken = null;
  }

  static Future<void> _spawnAnalysisProcess(
    String hash, {
    String? fileName,
    Rect? initialRect,
    void Function()? onExit,
  }) async {
    final key = _analysisProcessKey(hash);
    if (_analysisProcesses.containsKey(key)) {
      log.info(
        '[WindowManager] analysis process for $key still running, activating',
      );
      _activateAnalysisProcess(key);
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

    _attachProcessLogs(process, 'AnalysisProcess:$key');

    process.exitCode.then((code) {
      log.info(
        '[WindowManager] analysis process for $key exited with code $code',
      );
      _analysisProcesses.remove(key);
      _analysisExitCodes[key] = code;
      onExit?.call();
    });
  }

  static Future<void> _spawnAnalysisWorkspaceProcess(
    List<AnalysisWindowRequest> windows, {
    void Function()? onExit,
  }) async {
    final key = _analysisProcessKey(
      'workspace:${windows.map((w) => w.hash).join('|')}',
    );
    if (_analysisProcesses.containsKey(key)) {
      log.info(
        '[WindowManager] analysis workspace for ${windows.length} tracks still running, activating',
      );
      _activateAnalysisProcess(key);
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

    _attachProcessLogs(process, 'AnalysisWorkspaceProcess');

    process.exitCode.then((code) {
      log.info('[WindowManager] analysis workspace exited with code $code');
      _analysisProcesses.remove(key);
      _analysisExitCodes[key] = code;
      onExit?.call();
    });
  }

  static void _attachProcessLogs(Process process, String tag) {
    process.stdout
        .transform(utf8.decoder)
        .listen(
          (data) => log.info('[$tag] stdout: $data'),
          onError: (Object error, StackTrace stack) {
            log.warning('[$tag] stdout read failed: $error');
          },
        );
    process.stderr
        .transform(utf8.decoder)
        .listen(
          (data) => log.warning('[$tag] stderr: $data'),
          onError: (Object error, StackTrace stack) {
            log.warning('[$tag] stderr read failed: $error');
          },
        );
  }

  static bool _activateAnalysisProcess(String key) {
    final process = _analysisProcesses[key];
    if (process == null) return false;
    final hwnds = Win32FFI.findWindowsByProcessId(
      process.pid,
      className: kMainWindowClass,
    );
    for (final hwnd in hwnds) {
      if (Win32FFI.restoreAndBringToFront(hwnd)) {
        log.info('[WindowManager] activated analysis window hwnd=$hwnd');
        return true;
      }
    }
    log.warning(
      '[WindowManager] no activatable analysis window found for pid=${process.pid}',
    );
    return false;
  }

  static String _analysisProcessKey(String fallback) =>
      analysisIpcPort != null ? 'workspace:ipc' : fallback;

  static Future<Rect> _computeAnalysisWindowRect() =>
      _computeWindowRect(_analysisWindowType);

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

  static Future<Rect> _computeWindowRect(String type) async {
    final (defaultW, defaultH) = switch (type) {
      _analysisWindowType => _analysisDefaultSize,
      _ => (800, 600),
    };

    final parentHwnd = _findMainWindowHwnd();
    if (parentHwnd != 0) {
      final parentRect = Win32FFI.getWindowRect(parentHwnd);
      final monitorArea = Win32FFI.getMonitorWorkArea(parentHwnd);
      final offset = type == _analysisWindowType
          ? Win32FFI.titleBarOffset()
          : 32;
      if (type == _analysisWindowType) {
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

    return Rect.fromLTWH(100, 100, defaultW.toDouble(), defaultH.toDouble());
  }
}
