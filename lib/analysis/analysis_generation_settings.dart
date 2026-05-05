import '../config/app_config.dart';

abstract class AnalysisGenerationSettings {
  int get maxCacheBytes;
}

class AppConfigAnalysisGenerationSettings
    implements AnalysisGenerationSettings {
  const AppConfigAnalysisGenerationSettings();

  @override
  int get maxCacheBytes {
    if (!AppConfig.isInitialized) return 0;
    return AppConfig.instance.analysisCacheMaxBytes;
  }
}
