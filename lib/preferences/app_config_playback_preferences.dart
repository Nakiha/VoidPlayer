import '../config/app_settings_repository.dart';
import 'playback_preferences.dart';

class AppConfigPlaybackPreferences implements PlaybackPreferences {
  final AppSettingsRepository settings;

  const AppConfigPlaybackPreferences(this.settings);

  @override
  SeekAfterJumpBehavior get seekAfterJumpBehavior =>
      settings.seekAfterJumpBehavior;

  @override
  DecodeMode get decodeMode => settings.decodeMode;

  @override
  ViewportPixelSizeMode get viewportPixelSizeMode =>
      settings.viewportPixelSizeMode;

  @override
  bool get useHardwareDecode => decodeMode.useHardwareDecode;
}
