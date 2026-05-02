import 'dart:convert';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:path/path.dart' as p;
import '../preferences/playback_preferences.dart';

/// Manages reading and writing `config.json` located next to the executable.
///
/// Structure:
/// ```json
/// {
///   "window": { "x": 100, "y": 200, "width": 1280, "height": 720 },
///   "shortcuts": { ... },
///   "preferences": { ... }
/// }
/// ```
class AppConfig {
  AppConfig._();

  static AppConfig? _instance;
  static bool get isInitialized => _instance != null;
  static AppConfig get instance => _instance!;

  late final File _file;
  Map<String, dynamic> _data = {};

  /// Initializes the config manager. Reads `config.json` from the exe
  /// directory; creates an empty one if it doesn't exist.
  static Future<void> initialize() async {
    final instance = AppConfig._();
    final exeDir = p.dirname(Platform.resolvedExecutable);
    instance._file = File(p.join(exeDir, 'config.json'));

    try {
      final content = await instance._file.readAsString();
      instance._data = jsonDecode(content) as Map<String, dynamic>? ?? {};
    } catch (_) {
      // File missing or corrupted — start fresh.
      instance._data = {};
    }

    _instance = instance;
  }

  /// Persists the current in-memory state to disk.
  Future<void> save() async {
    try {
      await _file.writeAsString(jsonEncode(_data));
    } catch (_) {
      // Best-effort: don't block shutdown on write failure.
    }
  }

  // ---------------------------------------------------------------------------
  // Window section
  // ---------------------------------------------------------------------------

  static const _windowKey = 'window';
  static const _rectKeys = ['x', 'y', 'width', 'height'];

  /// Returns the saved window [Rect], or `null` if not stored.
  Rect? get windowRect {
    final w = _data[_windowKey];
    if (w is! Map<String, dynamic>) return null;
    if (!_rectKeys.every((k) => w[k] is num)) return null;
    return Rect.fromLTWH(
      (w['x'] as num).toDouble(),
      (w['y'] as num).toDouble(),
      (w['width'] as num).toDouble(),
      (w['height'] as num).toDouble(),
    );
  }

  /// Saves a window [Rect] to config. Pass `null` to clear.
  set windowRect(Rect? rect) {
    if (rect == null) {
      _data.remove(_windowKey);
    } else {
      _data[_windowKey] = {
        'x': rect.left,
        'y': rect.top,
        'width': rect.width,
        'height': rect.height,
      };
    }
  }

  // ---------------------------------------------------------------------------
  // Generic access for future sections (shortcuts, preferences, …)
  // ---------------------------------------------------------------------------

  /// Returns a section map, creating it if absent.
  Map<String, dynamic> section(String name) {
    return _data.putIfAbsent(name, () => <String, dynamic>{})
        as Map<String, dynamic>;
  }

  // ---------------------------------------------------------------------------
  // Preferences section
  // ---------------------------------------------------------------------------

  static const _preferencesKey = 'preferences';
  static const _analysisCacheMaxBytesKey = 'analysisCacheMaxBytes';
  static const _themeModeKey = 'themeMode';
  static const _accentColorModeKey = 'accentColorMode';
  static const _customAccentColorKey = 'customAccentColor';
  static const _seekAfterJumpBehaviorKey = 'seekAfterJumpBehavior';
  static const _defaultAnalysisCacheMaxBytes = 1024 * 1024 * 1024;

  /// Maximum analysis cache size in bytes. A value of 0 means unlimited.
  int get analysisCacheMaxBytes {
    final value = section(_preferencesKey)[_analysisCacheMaxBytesKey];
    if (value is num && value >= 0) return value.toInt();
    return _defaultAnalysisCacheMaxBytes;
  }

  set analysisCacheMaxBytes(int value) {
    section(_preferencesKey)[_analysisCacheMaxBytesKey] = value < 0 ? 0 : value;
  }

  String get themeModePreference {
    final value = section(_preferencesKey)[_themeModeKey];
    return value == 'light' || value == 'dark' ? value as String : 'system';
  }

  set themeModePreference(String value) {
    section(_preferencesKey)[_themeModeKey] =
        value == 'light' || value == 'dark' ? value : 'system';
  }

  String get accentColorPreference {
    final value = section(_preferencesKey)[_accentColorModeKey];
    return value == 'custom' ? 'custom' : 'system';
  }

  set accentColorPreference(String value) {
    section(_preferencesKey)[_accentColorModeKey] = value == 'custom'
        ? 'custom'
        : 'system';
  }

  int get customAccentColorValue {
    final value = section(_preferencesKey)[_customAccentColorKey];
    if (value is num) return value.toInt();
    return 0xFF0078D4;
  }

  set customAccentColorValue(int value) {
    section(_preferencesKey)[_customAccentColorKey] = value;
  }

  SeekAfterJumpBehavior get seekAfterJumpBehavior {
    final value = section(_preferencesKey)[_seekAfterJumpBehaviorKey];
    return SeekAfterJumpBehavior.fromStorage(value is String ? value : '');
  }

  set seekAfterJumpBehavior(SeekAfterJumpBehavior value) {
    section(_preferencesKey)[_seekAfterJumpBehaviorKey] = value.storageValue;
  }
}
