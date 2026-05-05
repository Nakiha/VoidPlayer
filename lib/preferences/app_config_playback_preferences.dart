import '../config/app_config.dart';
import 'playback_preferences.dart';

class AppConfigPlaybackPreferences implements PlaybackPreferences {
  const AppConfigPlaybackPreferences();

  @override
  SeekAfterJumpBehavior get seekAfterJumpBehavior =>
      AppConfig.instance.seekAfterJumpBehavior;
}
