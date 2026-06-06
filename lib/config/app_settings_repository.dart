import 'dart:ui';

import '../preferences/playback_preferences.dart';
import 'app_config.dart';

abstract class AppSettingsRepository {
  Rect? get windowRect;
  set windowRect(Rect? rect);

  int get analysisCacheMaxBytes;
  set analysisCacheMaxBytes(int value);

  String get themeModePreference;
  set themeModePreference(String value);

  String get accentColorPreference;
  set accentColorPreference(String value);

  int get customAccentColorValue;
  set customAccentColorValue(int value);

  SeekAfterJumpBehavior get seekAfterJumpBehavior;
  set seekAfterJumpBehavior(SeekAfterJumpBehavior value);

  DecodeMode get decodeMode;
  set decodeMode(DecodeMode value);

  ViewportPixelSizeMode get viewportPixelSizeMode;
  set viewportPixelSizeMode(ViewportPixelSizeMode value);

  DefaultAudioPlaybackPolicy get defaultAudioPlaybackPolicy;
  set defaultAudioPlaybackPolicy(DefaultAudioPlaybackPolicy value);

  PerformanceAlertPolicy get performanceAlertPolicy;
  set performanceAlertPolicy(PerformanceAlertPolicy value);

  Map<String, String> get securityScopedBookmarks;
  set securityScopedBookmarks(Map<String, String> value);

  Future<void> save();
}

class AppConfigSettingsRepository implements AppSettingsRepository {
  final AppConfig config;

  const AppConfigSettingsRepository(this.config);

  @override
  Rect? get windowRect => config.windowRect;

  @override
  set windowRect(Rect? rect) {
    config.windowRect = rect;
  }

  @override
  int get analysisCacheMaxBytes => config.analysisCacheMaxBytes;

  @override
  set analysisCacheMaxBytes(int value) {
    config.analysisCacheMaxBytes = value;
  }

  @override
  String get themeModePreference => config.themeModePreference;

  @override
  set themeModePreference(String value) {
    config.themeModePreference = value;
  }

  @override
  String get accentColorPreference => config.accentColorPreference;

  @override
  set accentColorPreference(String value) {
    config.accentColorPreference = value;
  }

  @override
  int get customAccentColorValue => config.customAccentColorValue;

  @override
  set customAccentColorValue(int value) {
    config.customAccentColorValue = value;
  }

  @override
  SeekAfterJumpBehavior get seekAfterJumpBehavior =>
      config.seekAfterJumpBehavior;

  @override
  set seekAfterJumpBehavior(SeekAfterJumpBehavior value) {
    config.seekAfterJumpBehavior = value;
  }

  @override
  DecodeMode get decodeMode => config.decodeMode;

  @override
  set decodeMode(DecodeMode value) {
    config.decodeMode = value;
  }

  @override
  ViewportPixelSizeMode get viewportPixelSizeMode =>
      config.viewportPixelSizeMode;

  @override
  set viewportPixelSizeMode(ViewportPixelSizeMode value) {
    config.viewportPixelSizeMode = value;
  }

  @override
  DefaultAudioPlaybackPolicy get defaultAudioPlaybackPolicy =>
      config.defaultAudioPlaybackPolicy;

  @override
  set defaultAudioPlaybackPolicy(DefaultAudioPlaybackPolicy value) {
    config.defaultAudioPlaybackPolicy = value;
  }

  @override
  PerformanceAlertPolicy get performanceAlertPolicy =>
      config.performanceAlertPolicy;

  @override
  set performanceAlertPolicy(PerformanceAlertPolicy value) {
    config.performanceAlertPolicy = value;
  }

  @override
  Map<String, String> get securityScopedBookmarks =>
      config.securityScopedBookmarks;

  @override
  set securityScopedBookmarks(Map<String, String> value) {
    config.securityScopedBookmarks = value;
  }

  @override
  Future<void> save() => config.save();
}
