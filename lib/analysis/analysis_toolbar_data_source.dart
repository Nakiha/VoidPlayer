import 'package:flutter/foundation.dart';

import '../config/app_settings_repository.dart';
import 'analysis_cache.dart';
import 'analysis_manager.dart';
import 'analysis_overlay.dart';

abstract class AnalysisToolbarDataSource implements Listenable {
  AnalysisState get state;
  AnalysisError? get error;
  String? get activeOverlayHash;
  AnalysisOverlayConfig get overlayConfig;
  AnalysisTrackGenerationStatus? statusForPath(String path);
  Future<AnalysisCacheSnapshot> snapshot();
  Future<Map<String, int>> currentBytesByHash(Set<String> hashes);
  String formatBytes(int bytes);
}

class DefaultAnalysisToolbarDataSource implements AnalysisToolbarDataSource {
  final AnalysisManager analysisManager;
  final AppSettingsRepository settings;

  const DefaultAnalysisToolbarDataSource({
    required this.analysisManager,
    required this.settings,
  });

  @override
  AnalysisState get state => analysisManager.state;

  @override
  AnalysisError? get error => analysisManager.error;

  @override
  String? get activeOverlayHash => analysisManager.activeOverlayHash;

  @override
  AnalysisOverlayConfig get overlayConfig => analysisManager.overlayConfig;

  @override
  void addListener(VoidCallback listener) {
    analysisManager.addListener(listener);
  }

  @override
  void removeListener(VoidCallback listener) {
    analysisManager.removeListener(listener);
  }

  @override
  AnalysisTrackGenerationStatus? statusForPath(String path) =>
      analysisManager.statusForPath(path);

  @override
  Future<AnalysisCacheSnapshot> snapshot() {
    return AnalysisCache.snapshot(maxBytes: settings.analysisCacheMaxBytes);
  }

  @override
  Future<Map<String, int>> currentBytesByHash(Set<String> hashes) {
    return AnalysisCache.currentBytesByHash(hashes);
  }

  @override
  String formatBytes(int bytes) => AnalysisCache.formatBytes(bytes);
}
