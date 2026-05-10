import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/action_registry.dart';
import 'package:void_player/analysis/analysis_cache.dart';
import 'package:void_player/analysis/analysis_manager.dart';
import 'package:void_player/analysis/analysis_overlay.dart';
import 'package:void_player/analysis/analysis_toolbar_data_source.dart';
import 'package:void_player/config/app_settings_repository.dart';
import 'package:void_player/preferences/playback_preferences.dart';
import 'package:void_player/startup_options.dart';
import 'package:void_player/windows/main/main_window_controller.dart';
import 'package:void_player/windows/main/main_window_platform.dart';
import 'package:void_player/windows/window_manager.dart' as app_window;

class _FakeMainWindowPlatform implements MainWindowPlatform {
  @override
  Future<Rect> getBounds() => Future.value(Rect.zero);

  @override
  Future<void> setFullScreen(bool fullScreen) => Future.value();
}

class _FakeAnalysisGenerationService implements AnalysisGenerationService {
  AnalysisOverlayConfig _config = const AnalysisOverlayConfig();

  @override
  String? get activeOverlayHash => null;

  @override
  bool get overlayPanelVisible => false;

  @override
  Set<int> get activeOverlayTrackFileIds => const {};

  @override
  AnalysisOverlayConfig get overlayConfig => _config;

  @override
  Future<String?> ensureGenerated(String videoPath) => Future.value(null);

  @override
  Future<bool> activateOverlay(
    String hash, {
    required String name,
    required String path,
    required int trackFileId,
  }) => Future.value(false);

  @override
  Future<bool> activateOverlayTracks(List<AnalysisOverlayTrackSource> tracks) =>
      Future.value(false);

  @override
  void updateOverlayConfig(AnalysisOverlayConfig config) {
    _config = config;
  }

  @override
  void deactivateOverlay() {}
}

class _FakePlaybackPreferences implements PlaybackPreferences {
  @override
  SeekAfterJumpBehavior get seekAfterJumpBehavior =>
      SeekAfterJumpBehavior.keepPreviousState;

  @override
  DecodeMode get decodeMode => DecodeMode.preferHardware;

  @override
  ViewportPixelSizeMode get viewportPixelSizeMode =>
      ViewportPixelSizeMode.uniformVideoPixels;

  @override
  bool get useHardwareDecode => decodeMode.useHardwareDecode;
}

class _FakeAppSettingsRepository implements AppSettingsRepository {
  @override
  Rect? windowRect;

  @override
  int analysisCacheMaxBytes = 0;

  @override
  String themeModePreference = 'system';

  @override
  String accentColorPreference = 'system';

  @override
  int customAccentColorValue = 0xFF0078D4;

  @override
  SeekAfterJumpBehavior seekAfterJumpBehavior =
      SeekAfterJumpBehavior.keepPreviousState;

  @override
  DecodeMode decodeMode = DecodeMode.preferHardware;

  @override
  ViewportPixelSizeMode viewportPixelSizeMode =
      ViewportPixelSizeMode.uniformVideoPixels;

  @override
  Future<void> save() => Future.value();
}

class _FakeAnalysisToolbarDataSource implements AnalysisToolbarDataSource {
  @override
  AnalysisState get state => AnalysisState.idle;

  @override
  AnalysisError? get error => null;

  @override
  String? get activeOverlayHash => null;

  @override
  bool get overlayPanelVisible => false;

  @override
  Set<int> get activeOverlayTrackFileIds => const {};

  @override
  AnalysisOverlayConfig get overlayConfig => const AnalysisOverlayConfig();

  @override
  void addListener(VoidCallback listener) {}

  @override
  void removeListener(VoidCallback listener) {}

  @override
  AnalysisTrackGenerationStatus? statusForPath(String path) => null;

  @override
  Future<AnalysisCacheSnapshot> snapshot() => Future.value(
    const AnalysisCacheSnapshot(
      path: '',
      totalBytes: 0,
      indexedBytes: 0,
      unindexedBytes: 0,
      maxBytes: 0,
      entries: [],
    ),
  );

  @override
  Future<Map<String, int>> currentBytesByHash(Set<String> hashes) =>
      Future.value(const {});

  @override
  String formatBytes(int bytes) => '$bytes B';
}

void main() {
  test('MainWindowController keeps injected platform services', () {
    final platformWindow = _FakeMainWindowPlatform();
    final analysisProcesses = app_window.AnalysisProcessManager();
    final analysisGeneration = _FakeAnalysisGenerationService();
    final analysisToolbarDataSource = _FakeAnalysisToolbarDataSource();
    final appSettings = _FakeAppSettingsRepository();
    final playbackPreferences = _FakePlaybackPreferences();
    final controller = MainWindowController(
      actionRegistry: ActionRegistry(),
      vsync: const TestVSync(),
      startupOptions: const StartupOptions(),
      mounted: () => true,
      platformWindow: platformWindow,
      analysisProcesses: analysisProcesses,
      analysisGeneration: analysisGeneration,
      analysisToolbarDataSource: analysisToolbarDataSource,
      appSettings: appSettings,
      playbackPreferences: playbackPreferences,
    );
    addTearDown(controller.dispose);

    expect(controller.platformWindow, same(platformWindow));
    expect(controller.analysisProcesses, same(analysisProcesses));
    expect(controller.analysisGeneration, same(analysisGeneration));
    expect(
      controller.analysisToolbarDataSource,
      same(analysisToolbarDataSource),
    );
    expect(controller.appSettings, same(appSettings));
    expect(controller.playbackPreferences, same(playbackPreferences));
  });
}
