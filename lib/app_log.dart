import 'dart:async';
import 'dart:collection';
import 'dart:convert';
import 'dart:io';
import 'dart:isolate';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:intl/intl.dart';
import 'package:logging/logging.dart';
import 'package:path/path.dart' as p;

import 'app_paths.dart';

/// Module names used in --log-level parsing.
enum LogModule {
  flutter('flutter'),
  native('native'),
  ffmpeg('ffmpeg');

  final String key;
  const LogModule(this.key);

  static LogModule? fromKey(String key) {
    final lower = key.toLowerCase();
    for (final m in LogModule.values) {
      if (m.key == lower) return m;
    }
    return null;
  }
}

/// Parsed log configuration for all modules.
class LogConfig {
  final Level flutter;
  final Level native;
  final Level ffmpeg;
  final String logsDir;
  final String processRole;
  final String logSession;

  const LogConfig({
    required this.flutter,
    required this.native,
    required this.ffmpeg,
    required this.logsDir,
    required this.processRole,
    required this.logSession,
  });

  /// Default config: INFO for all modules, logs in the resolved app data root.
  static LogConfig defaultsFor(List<String> args) {
    return LogConfig(
      flutter: Level.INFO,
      native: Level.INFO,
      ffmpeg: Level.INFO,
      logsDir: AppPaths.current.logsDir,
      processRole: 'main',
      logSession: DateFormat('yyyy-MM-dd_HHmmss_SSS').format(DateTime.now()),
    );
  }

  /// Parse --log-level argument like "flutter=DEBUG,native=INFO,ffmpeg=INFO"
  static LogConfig parse(List<String> args) {
    var config = defaultsFor(args);

    for (final arg in args) {
      if (!arg.startsWith('--log-level=')) continue;
      final value = arg.substring('--log-level='.length);
      for (final part in value.split(',')) {
        final eq = part.indexOf('=');
        if (eq < 0) continue;
        final key = part.substring(0, eq).trim();
        final levelStr = part.substring(eq + 1).trim();
        final level = _parseLevel(levelStr);
        if (level == null) continue;
        final module = LogModule.fromKey(key);
        if (module == null) continue;
        config = config._withLevel(module, level);
      }
    }

    return config;
  }

  LogConfig _withLevel(LogModule module, Level level) {
    switch (module) {
      case LogModule.flutter:
        return LogConfig(
          flutter: level,
          native: native,
          ffmpeg: ffmpeg,
          logsDir: logsDir,
          processRole: processRole,
          logSession: logSession,
        );
      case LogModule.native:
        return LogConfig(
          flutter: flutter,
          native: level,
          ffmpeg: ffmpeg,
          logsDir: logsDir,
          processRole: processRole,
          logSession: logSession,
        );
      case LogModule.ffmpeg:
        return LogConfig(
          flutter: flutter,
          native: native,
          ffmpeg: level,
          logsDir: logsDir,
          processRole: processRole,
          logSession: logSession,
        );
    }
  }

  LogConfig _withLogsDir(String value) {
    return LogConfig(
      flutter: flutter,
      native: native,
      ffmpeg: ffmpeg,
      logsDir: value,
      processRole: processRole,
      logSession: logSession,
    );
  }

  /// Returns the native log level as spdlog string.
  String get nativeLevelName {
    if (native == Level.ALL) return 'trace';
    if (native <= Level.FINEST) return 'trace';
    if (native <= Level.FINER) return 'debug';
    if (native <= Level.FINE) return 'debug';
    if (native <= Level.CONFIG) return 'info';
    if (native <= Level.INFO) return 'info';
    if (native <= Level.WARNING) return 'warn';
    if (native <= Level.SEVERE) return 'err';
    return 'off';
  }

  String get flutterLogPrefix => 'void_player_${processRole}_$pid';
  String get nativeLogFileName =>
      'native_${processRole}_${pid}_$logSession.log';
}

// ---------------------------------------------------------------------------
// Logger setup
// ---------------------------------------------------------------------------

late LogConfig _logConfig;
late Logger _root;
StreamSubscription<LogRecord>? _rootSubscription;
bool _loggingInitialized = false;
RawReceivePort? _isolateErrorPort;
FlutterExceptionHandler? _previousFlutterErrorHandler;
FlutterExceptionHandler? _installedFlutterErrorHandler;

/// The parsed log config. Available after [initLogging].
LogConfig get logConfig => _logConfig;

/// The root Logger. Available after [initLogging].
Logger get log => _root;

/// Returns a component logger that inherits the configured Flutter level.
///
/// Prefer this over embedding a component name in every message. The formatter
/// emits the logger name as a stable, searchable field.
Logger appLogger(String component) => Logger(component);

void logFine(String message, [Object? error, StackTrace? stackTrace]) {
  if (!_loggingInitialized) return;
  _root.fine(message, error, stackTrace);
}

void logWarning(String message, [Object? error, StackTrace? stackTrace]) {
  if (!_loggingInitialized) return;
  _root.warning(message, error, stackTrace);
}

/// Initialize logging system. Call once at app startup, before runApp.
///
/// [args] are the CLI arguments from `getDartEntryPointArguments()`.
/// Returns the resolved [LogConfig] so callers can pass native level to plugin.
Future<LogConfig> initLogging(
  List<String> args, {
  String? logsDirOverride,
}) async {
  await _resetFileSink();
  await _rootSubscription?.cancel();
  _rootSubscription = null;
  _removeGlobalErrorHooks();
  _fileSinkFailureReported = false;

  _logConfig = logsDirOverride == null
      ? LogConfig.parse(args)
      : LogConfig.parse(args)._withLogsDir(logsDirOverride);

  final logsDir = Directory(_logConfig.logsDir);
  if (!await logsDir.exists()) {
    await logsDir.create(recursive: true);
  }

  _cleanOldLogs(_logConfig.logsDir);

  _root = Logger.root;
  _loggingInitialized = true;
  _root.level = _logConfig.flutter;
  _rootSubscription = _root.onRecord.listen(_dispatchRecord);

  _previousFlutterErrorHandler = FlutterError.onError;
  _installedFlutterErrorHandler = (details) {
    _root.severe('Unhandled Flutter error', details.exception, details.stack);
  };
  FlutterError.onError = _installedFlutterErrorHandler;

  _isolateErrorPort = RawReceivePort((Object? payload) {
    final pair = payload is List<dynamic> ? payload : const <dynamic>[];
    final error = pair.isNotEmpty ? pair.first : payload;
    final stack = pair.length > 1
        ? StackTrace.fromString('${pair.last}')
        : null;
    _root.severe('Unhandled isolate error', error, stack);
  });
  Isolate.current.addErrorListener(_isolateErrorPort!.sendPort);

  _root.info(
    'Logging initialized: flutter=${_logConfig.flutter.name}, '
    'native=${_logConfig.nativeLevelName}, ffmpeg=${_logConfig.ffmpeg.name}, '
    'logsDir=${_logConfig.logsDir}, role=${_logConfig.processRole}, pid=$pid',
  );

  // Forward native log config to C++ side via MethodChannel.
  // Fire-and-forget — native plugin already initialized with defaults
  // on construction, this just updates the level if --log-level changed it.
  unawaited(_configureNativeLogging(_logConfig));

  return _logConfig;
}

/// Send parsed native log config to the C++ plugin via MethodChannel.
/// Fire-and-forget: if the plugin isn't registered yet or the call fails,
/// native logging keeps its default config (initialized in plugin constructor).
Future<void> _configureNativeLogging(LogConfig config) async {
  try {
    const channel = MethodChannel('video_renderer');
    await channel.invokeMethod<void>('initLogging', {
      'logLevel': config.nativeLevelName,
      'logsDir': config.logsDir,
      'logFileName': config.nativeLogFileName,
      'processRole': config.processRole,
      'processId': pid,
    });
  } on MissingPluginException catch (error, stackTrace) {
    appLogger('Logging').fine(
      'Native logging configuration unavailable; using native defaults',
      error,
      stackTrace,
    );
  } on FlutterError catch (error, stackTrace) {
    if (error.message.contains('Binding has not yet been initialized')) {
      appLogger('Logging').fine(
        'Native logging configuration deferred until bindings initialize',
        error,
        stackTrace,
      );
      return;
    }
    appLogger('Logging').warning(
      'Native logging configuration failed; using native defaults',
      error,
      stackTrace,
    );
  } on Object catch (error, stackTrace) {
    appLogger('Logging').warning(
      'Native logging configuration failed; using native defaults',
      error,
      stackTrace,
    );
  }
}

// ---------------------------------------------------------------------------
// Single dispatch: format once, send to both file and console
// ---------------------------------------------------------------------------

const String _kLogPrefix = 'void_player_';
const String _kNativeLogPrefix = 'native_';
const int _kMaxFileSize = 5 * 1024 * 1024; // 5 MB
const int _kMaxFiles = 30;

RandomAccessFile? _raf;
int _currentFileSize = 0;
String? _cachedDateStr;
final Queue<String> _pendingFileLines = Queue<String>();
bool _fileFlushScheduled = false;
bool _fileFlushInProgress = false;
Completer<void>? _fileFlushCompleter;
bool _fileSinkFailureReported = false;

void _dispatchRecord(LogRecord record) {
  final line = _formatRecord(record);
  _writeFile(line);
  _writeConsole(line, record.level);
}

// ---------------------------------------------------------------------------
// File handler with rotation
// ---------------------------------------------------------------------------

void _writeFile(String line) {
  try {
    _pendingFileLines.add(line);
    _scheduleFileFlush();
  } catch (_) {
    // Never let logging crash the app
  }
}

void _scheduleFileFlush() {
  if (_fileFlushScheduled || _fileFlushInProgress) return;
  _fileFlushScheduled = true;
  scheduleMicrotask(() {
    _fileFlushScheduled = false;
    unawaited(_flushFileQueue());
  });
}

Future<void> flushLogFile() async {
  while (_pendingFileLines.isNotEmpty ||
      _fileFlushScheduled ||
      _fileFlushInProgress) {
    await _flushFileQueue();
    if (_fileFlushScheduled && !_fileFlushInProgress) {
      await Future<void>.delayed(Duration.zero);
    }
  }
}

/// Flushes pending records, closes the file handle, and stops root dispatch.
///
/// This is primarily useful for deterministic process and test teardown on
/// Windows, where an open log handle prevents its containing directory from
/// being removed.
Future<void> shutdownLogging() async {
  await _rootSubscription?.cancel();
  _rootSubscription = null;
  _removeGlobalErrorHooks();
  await _resetFileSink();
  _loggingInitialized = false;
}

void _removeGlobalErrorHooks() {
  final port = _isolateErrorPort;
  if (port != null) {
    Isolate.current.removeErrorListener(port.sendPort);
    port.close();
    _isolateErrorPort = null;
  }
  if (identical(FlutterError.onError, _installedFlutterErrorHandler)) {
    FlutterError.onError = _previousFlutterErrorHandler;
  }
  _installedFlutterErrorHandler = null;
  _previousFlutterErrorHandler = null;
}

Future<void> _flushFileQueue() {
  if (_fileFlushInProgress) {
    return _fileFlushCompleter?.future ?? Future<void>.value();
  }
  final completer = Completer<void>();
  _fileFlushCompleter = completer;
  _fileFlushInProgress = true;

  Future<void>(() async {
    try {
      while (_pendingFileLines.isNotEmpty) {
        await _ensureLogOpen();
        final file = _raf;
        if (file == null) return;

        final buffer = StringBuffer();
        var count = 0;
        while (_pendingFileLines.isNotEmpty && count < 256) {
          buffer.writeln(_pendingFileLines.removeFirst());
          count++;
        }
        final bytes = utf8.encode(buffer.toString());
        _currentFileSize += bytes.length;
        await file.writeFrom(bytes);

        if (_currentFileSize >= _kMaxFileSize) {
          await file.close();
          if (identical(_raf, file)) {
            _raf = null;
            _cachedDateStr = null;
          }
          _cleanOldLogs(_logConfig.logsDir);
        }
      }
    } catch (error, stackTrace) {
      _pendingFileLines.clear();
      _reportFileSinkFailure(error, stackTrace);
    } finally {
      _fileFlushInProgress = false;
      _fileFlushCompleter = null;
      completer.complete();
      if (_pendingFileLines.isNotEmpty) _scheduleFileFlush();
    }
  });

  return completer.future;
}

Future<void> _resetFileSink() async {
  try {
    await flushLogFile();
    await _raf?.close();
  } catch (error, stackTrace) {
    // Never let logging teardown crash tests or app startup.
    _reportFileSinkFailure(error, stackTrace);
  } finally {
    _raf = null;
    _currentFileSize = 0;
    _cachedDateStr = null;
    _pendingFileLines.clear();
    _fileFlushScheduled = false;
    _fileFlushInProgress = false;
    _fileFlushCompleter = null;
  }
}

void _reportFileSinkFailure(Object error, StackTrace stackTrace) {
  if (_fileSinkFailureReported) return;
  _fileSinkFailureReported = true;
  _stderrWriter.write(
    '[VoidPlayer][Logging] file sink failed; console logging remains active: '
    '$error\n$stackTrace',
  );
}

Future<void> _ensureLogOpen() async {
  final now = DateTime.now();
  final dateStr = DateFormat('yyyy-MM-dd').format(now);

  if (_cachedDateStr == dateStr && _raf != null) return;

  await _raf?.close();
  _raf = null;
  _cachedDateStr = dateStr;

  final logName = '${_logConfig.flutterLogPrefix}_$dateStr.log';
  final logPath = p.join(_logConfig.logsDir, logName);

  final file = File(logPath);
  _raf = await file.open(mode: FileMode.append);
  _currentFileSize = await _raf!.length();
}

void _cleanOldLogs(String logsDir) {
  final dir = Directory(logsDir);
  if (!dir.existsSync()) return;

  final List<FileSystemEntity> entities;
  try {
    entities = dir.listSync();
  } on FileSystemException {
    // Another process may remove or rotate the directory concurrently.
    return;
  }

  final logFiles = <MapEntry<File, DateTime>>[];
  for (final entity in entities) {
    final basename = p.basename(entity.path);
    if (entity is File &&
        (basename.startsWith(_kLogPrefix) ||
            basename.startsWith(_kNativeLogPrefix))) {
      try {
        logFiles.add(MapEntry(entity, entity.lastModifiedSync()));
      } on FileSystemException {
        // Cross-process rotation can remove an entry after listSync().
      }
    }
  }

  if (logFiles.length <= _kMaxFiles) return;

  logFiles.sort((a, b) => a.value.compareTo(b.value));

  final toDelete = logFiles.length - _kMaxFiles;
  for (int i = 0; i < toDelete; i++) {
    try {
      logFiles[i].key.deleteSync();
    } catch (_) {}
  }
}

// ---------------------------------------------------------------------------
// Console handler (safe for GUI mode)
// ---------------------------------------------------------------------------

class _StdWriter {
  final IOSink _sink;
  bool _checked = false;
  bool _available = false;

  _StdWriter(this._sink);

  void write(String line) {
    if (!_checked) {
      _checked = true;
      try {
        _sink.write('');
        _sink.flush();
        _available = true;
      } catch (_) {
        _available = false;
      }
    }
    if (_available) {
      try {
        _sink.writeln(line);
      } catch (_) {
        _available = false;
      }
    }
  }
}

final _stdoutWriter = _StdWriter(stdout);
final _stderrWriter = _StdWriter(stderr);

void _writeConsole(String line, Level level) {
  if (level >= Level.WARNING) {
    _stderrWriter.write(line);
  } else {
    _stdoutWriter.write(line);
  }
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

final DateFormat _timeFmt = DateFormat('yyyy-MM-dd HH:mm:ss.SSS');

String _formatRecord(LogRecord record) {
  final ts = _timeFmt.format(record.time);
  final level = record.level.name.padRight(7);
  final logger = record.loggerName != '' ? ' [${record.loggerName}]' : '';
  final msg = record.message;
  final error = record.error != null ? '\n  Error: ${record.error}' : '';
  final stack = record.stackTrace != null
      ? '\n  Stack: ${record.stackTrace}'
      : '';
  return '[$ts] $level$logger: $msg$error$stack';
}

Level? _parseLevel(String s) {
  switch (s.toUpperCase()) {
    case 'TRACE':
    case 'ALL':
      return Level.ALL;
    case 'FINEST':
      return Level.FINEST;
    case 'FINER':
    case 'DEBUG':
      return Level.FINER;
    case 'FINE':
      return Level.FINE;
    case 'CONFIG':
      return Level.CONFIG;
    case 'INFO':
      return Level.INFO;
    case 'WARNING':
    case 'WARN':
      return Level.WARNING;
    case 'SEVERE':
    case 'ERROR':
      return Level.SEVERE;
    case 'SHOUT':
    case 'FATAL':
    case 'OFF':
      return Level.SHOUT;
    default:
      return null;
  }
}
