import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:isolate';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:intl/intl.dart';
import 'package:logging/logging.dart';
import 'package:path/path.dart' as p;

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

  const LogConfig({
    required this.flutter,
    required this.native,
    required this.ffmpeg,
    required this.logsDir,
  });

  /// Default config: INFO for all modules, logs next to exe.
  static LogConfig get defaults {
    final exeDir = _exeDir();
    return LogConfig(
      flutter: Level.INFO,
      native: Level.INFO,
      ffmpeg: Level.INFO,
      logsDir: p.join(exeDir, 'logs'),
    );
  }

  /// Parse --log-level argument like "flutter=DEBUG,native=INFO,ffmpeg=INFO"
  static LogConfig parse(List<String> args) {
    var config = defaults;

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
        return LogConfig(flutter: level, native: native, ffmpeg: ffmpeg, logsDir: logsDir);
      case LogModule.native:
        return LogConfig(flutter: flutter, native: level, ffmpeg: ffmpeg, logsDir: logsDir);
      case LogModule.ffmpeg:
        return LogConfig(flutter: flutter, native: native, ffmpeg: level, logsDir: logsDir);
    }
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
}

// ---------------------------------------------------------------------------
// Logger setup
// ---------------------------------------------------------------------------

late LogConfig _logConfig;
late Logger _root;

/// The parsed log config. Available after [initLogging].
LogConfig get logConfig => _logConfig;

/// The root Logger. Available after [initLogging].
Logger get log => _root;

/// Initialize logging system. Call once at app startup, before runApp.
///
/// [args] are the CLI arguments from `getDartEntryPointArguments()`.
/// Returns the resolved [LogConfig] so callers can pass native level to plugin.
Future<LogConfig> initLogging(List<String> args) async {
  _logConfig = LogConfig.parse(args);

  final logsDir = Directory(_logConfig.logsDir);
  if (!await logsDir.exists()) {
    await logsDir.create(recursive: true);
  }

  _cleanOldLogs(_logConfig.logsDir);

  _root = Logger.root;
  _root.level = _logConfig.flutter;
  _root.onRecord.listen(_dispatchRecord);

  FlutterError.onError = (details) {
    _root.warning('Flutter error: ${details.exceptionAsString()}\n${details.stack}');
  };

  Isolate.current.addErrorListener(
    RawReceivePort((pair) async {
      final List<dynamic> errorAndStack = pair as List<dynamic>;
      _root.severe('Isolate error: ${errorAndStack.first}\n${errorAndStack.last}');
    }).sendPort,
  );

  _root.info('Logging initialized: flutter=${_logConfig.flutter.name}, '
      'native=${_logConfig.nativeLevelName}, ffmpeg=${_logConfig.ffmpeg.name}, '
      'logsDir=${_logConfig.logsDir}');

  // Forward native log config to C++ side via MethodChannel.
  // Fire-and-forget — native plugin already initialized with defaults
  // on construction, this just updates the level if --log-level changed it.
  _configureNativeLogging();

  return _logConfig;
}

/// Send parsed native log config to the C++ plugin via MethodChannel.
/// Fire-and-forget: if the plugin isn't registered yet or the call fails,
/// native logging keeps its default config (initialized in plugin constructor).
Future<void> _configureNativeLogging() async {
  try {
    const channel = MethodChannel('video_renderer');
    await channel.invokeMethod<void>('initLogging', {
      'logLevel': _logConfig.nativeLevelName,
      'logsDir': _logConfig.logsDir,
    });
  } catch (_) {
    // Plugin not registered yet or not available — native defaults are fine.
  }
}

// ---------------------------------------------------------------------------
// Single dispatch: format once, send to both file and console
// ---------------------------------------------------------------------------

const String _kLogPrefix = 'void_player_';
const int _kMaxFileSize = 5 * 1024 * 1024; // 5 MB
const int _kMaxFiles = 5;

RandomAccessFile? _raf;
int _currentFileSize = 0;
String? _cachedDateStr;

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
    _ensureLogOpen();
    if (_raf == null) return;

    final bytes = utf8.encode('$line\n');
    _currentFileSize += bytes.length;
    _raf!.writeFromSync(bytes);

    if (_currentFileSize >= _kMaxFileSize) {
      _raf!.closeSync();
      _raf = null;
      _cachedDateStr = null;
      _cleanOldLogs(_logConfig.logsDir);
    }
  } catch (_) {
    // Never let logging crash the app
  }
}

void _ensureLogOpen() {
  final now = DateTime.now();
  final dateStr = DateFormat('yyyy-MM-dd').format(now);

  if (_cachedDateStr == dateStr && _raf != null) return;

  _raf?.closeSync();
  _raf = null;
  _cachedDateStr = dateStr;

  final logName = '$_kLogPrefix$dateStr.log';
  final logPath = p.join(_logConfig.logsDir, logName);

  final file = File(logPath);
  _raf = file.openSync(mode: FileMode.append);
  _currentFileSize = _raf!.lengthSync();
}

void _cleanOldLogs(String logsDir) {
  final dir = Directory(logsDir);
  if (!dir.existsSync()) return;

  final logFiles = <File>[];
  for (final entity in dir.listSync()) {
    if (entity is File && p.basename(entity.path).startsWith(_kLogPrefix)) {
      logFiles.add(entity);
    }
  }

  if (logFiles.length <= _kMaxFiles) return;

  logFiles.sort((a, b) =>
      a.lastModifiedSync().compareTo(b.lastModifiedSync()));

  final toDelete = logFiles.length - _kMaxFiles;
  for (int i = 0; i < toDelete; i++) {
    try {
      logFiles[i].deleteSync();
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
  final stack =
      record.stackTrace != null ? '\n  Stack: ${record.stackTrace}' : '';
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

/// Get the directory where the exe lives.
String _exeDir() {
  final exePath = Platform.resolvedExecutable;
  return p.dirname(exePath);
}
