import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/action_registry.dart';
import 'package:void_player/analysis/analysis_cache.dart';
import 'package:void_player/analysis/analysis_manager.dart';
import 'package:void_player/analysis/analysis_overlay.dart';
import 'package:void_player/analysis/analysis_toolbar_data_source.dart';
import 'package:void_player/config/app_settings_repository.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/marks/quick_mark_persistence.dart';
import 'package:void_player/platform/analysis_process_host.dart';
import 'package:void_player/platform/main_window_platform.dart';
import 'package:void_player/platform/platform_capabilities.dart';
import 'package:void_player/preferences/playback_preferences.dart';
import 'package:void_player/startup_options.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/windows/main/main_window_controller.dart';

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
  int get overlayPresentationRevision => 0;

  @override
  AnalysisTrackGenerationStatus? statusForPath(String path) => null;

  @override
  bool supportsOverlayForHash(String hash) => true;

  @override
  Future<String?> ensureGenerated(String videoPath) => Future.value(null);

  @override
  Future<String?> ensureGeneratedAndLoaded(String videoPath) =>
      Future.value(null);

  @override
  Future<bool> ensureOverlayChunk(
    String hash, {
    required String videoPath,
    int? analysisFrameIndex,
    int? sourcePacketIndex,
    int? sourcePacketSize,
    int? sourcePacketPos,
    int? sourcePacketPtsUs,
    int? sourcePacketDtsUs,
    int? presentedPtsUs,
    int? presentedDtsUs,
  }) => Future.value(false);

  @override
  Future<bool> activateOverlay(
    String hash, {
    required String name,
    required String path,
    required int trackFileId,
    int? analysisFrameIndex,
    int? sourcePacketIndex,
    int? sourcePacketSize,
    int? sourcePacketPos,
    int? sourcePacketPtsUs,
    int? sourcePacketDtsUs,
    int? presentedPtsUs,
    int? presentedDtsUs,
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
  DefaultAudioPlaybackPolicy get defaultAudioPlaybackPolicy =>
      DefaultAudioPlaybackPolicy.muted;

  @override
  PerformanceAlertPolicy get performanceAlertPolicy =>
      PerformanceAlertPolicy.sustained;

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
  DefaultAudioPlaybackPolicy defaultAudioPlaybackPolicy =
      DefaultAudioPlaybackPolicy.muted;

  @override
  PerformanceAlertPolicy performanceAlertPolicy =
      PerformanceAlertPolicy.sustained;

  @override
  Map<String, String> securityScopedBookmarks = {};

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
  bool supportsOverlayForHash(String hash) => true;

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
  TestWidgetsFlutterBinding.ensureInitialized();

  test('MainWindowController keeps injected platform services', () {
    final platformWindow = _FakeMainWindowPlatform();
    final analysisProcesses = UnsupportedAnalysisProcessHost();
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

  test(
    'macOS phase capabilities hide analysis window but keep overlay entry',
    () {
      final controller = MainWindowController(
        actionRegistry: ActionRegistry(),
        vsync: const TestVSync(),
        startupOptions: const StartupOptions(),
        mounted: () => true,
        platformCapabilities: PlatformCapabilities.macOSPhase1,
        analysisGeneration: _FakeAnalysisGenerationService(),
        analysisToolbarDataSource: _FakeAnalysisToolbarDataSource(),
        appSettings: _FakeAppSettingsRepository(),
        playbackPreferences: _FakePlaybackPreferences(),
      );
      addTearDown(controller.dispose);

      controller.trackManager.addTrack(
        const TrackInfo(
          fileId: 1,
          slot: 0,
          path: '/tmp/video.mp4',
          width: 320,
          height: 180,
        ),
      );

      expect(controller.viewModel.media.analysisEnabled, isFalse);
      expect(controller.viewModel.media.analysisOverlayEnabled, isTrue);
    },
  );

  test(
    'quick mark view model keeps all marks separate from viewport marks',
    () {
      final controller = MainWindowController(
        actionRegistry: ActionRegistry(),
        vsync: const TestVSync(),
        startupOptions: const StartupOptions(),
        mounted: () => true,
        analysisGeneration: _FakeAnalysisGenerationService(),
        analysisToolbarDataSource: _FakeAnalysisToolbarDataSource(),
        appSettings: _FakeAppSettingsRepository(),
        playbackPreferences: _FakePlaybackPreferences(),
      );
      addTearDown(controller.dispose);

      controller.trackManager.addTrack(
        const TrackInfo(
          fileId: 1,
          slot: 0,
          path: '/tmp/video.mp4',
          width: 320,
          height: 180,
        ),
      );
      controller.stateStore
        ..setQuickMarks(const [
          QuickMark(
            id: 1,
            anchor: QuickMarkAnchor(
              fileId: 1,
              ptsUs: 1000,
              dtsUs: 1000,
              durationUs: 1000,
            ),
            sourceRect: Rect.fromLTRB(0.1, 0.1, 0.2, 0.2),
          ),
          QuickMark(
            id: 2,
            anchor: QuickMarkAnchor(
              fileId: 1,
              ptsUs: 4000,
              dtsUs: 4000,
              durationUs: 1000,
            ),
            sourceRect: Rect.fromLTRB(0.3, 0.3, 0.4, 0.4),
          ),
        ])
        ..setSelectedQuickMarkId(2)
        ..setPolledPlaybackState(
          1000,
          10000,
          false,
          presentedFrameAnchors: const {
            1: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
          },
        );

      final viewModel = controller.viewModel;

      expect(viewModel.viewport.quickMarks.map((mark) => mark.id), const [1]);
      expect(viewModel.viewport.selectedQuickMarkId, isNull);
      expect(viewModel.marks.allMarks.map((mark) => mark.id), const [1, 2]);
      expect(viewModel.marks.visibleMarkIds, const {1});
      expect(viewModel.marks.selectedMarkId, 2);
      expect(viewModel.marks.tracksByFileId[1]?.path, '/tmp/video.mp4');
    },
  );

  test(
    'quick mark repository loads marks for runtime track file ids',
    () async {
      final repository = _FakeQuickMarkRepository(
        loadedMarks: const [
          QuickMark(
            id: 8,
            anchor: QuickMarkAnchor(fileId: 99, ptsUs: 1000, dtsUs: 1000),
            sourceRect: Rect.fromLTRB(0.1, 0.1, 0.2, 0.2),
            text: 'persisted',
          ),
        ],
      );
      final controller = MainWindowController(
        actionRegistry: ActionRegistry(),
        vsync: const TestVSync(),
        startupOptions: const StartupOptions(),
        mounted: () => true,
        analysisGeneration: _FakeAnalysisGenerationService(),
        analysisToolbarDataSource: _FakeAnalysisToolbarDataSource(),
        appSettings: _FakeAppSettingsRepository(),
        playbackPreferences: _FakePlaybackPreferences(),
        quickMarkRepository: repository,
      );
      addTearDown(controller.dispose);
      controller.start();

      controller.trackManager.addTrack(
        const TrackInfo(
          fileId: 4,
          slot: 0,
          path: '/tmp/video.mp4',
          width: 320,
          height: 180,
        ),
      );

      await Future<void>.delayed(Duration.zero);
      await Future<void>.delayed(Duration.zero);

      expect(repository.loadedRefs.single.fileId, 4);
      expect(repository.savedRefs, isEmpty);
      expect(controller.viewModel.marks.allMarks, hasLength(1));
      expect(controller.viewModel.marks.allMarks.single.fileId, 4);
      expect(controller.viewModel.marks.allMarks.single.text, 'persisted');
    },
  );
}

class _FakeQuickMarkRepository implements QuickMarkRepository {
  final List<QuickMark> loadedMarks;
  List<QuickMarkMediaRef> loadedRefs = const [];
  List<QuickMarkMediaRef> savedRefs = const [];

  _FakeQuickMarkRepository({required this.loadedMarks});

  @override
  Future<List<QuickMark>> loadForMediaRefs(
    List<QuickMarkMediaRef> mediaRefs,
  ) async {
    loadedRefs = List.unmodifiable(mediaRefs);
    return loadedMarks
        .map(
          (mark) => mark.copyWith(
            anchor: mark.anchor.copyWith(fileId: mediaRefs.first.fileId),
          ),
        )
        .toList(growable: false);
  }

  @override
  Future<void> saveForMediaRefs(
    List<QuickMarkMediaRef> mediaRefs,
    List<QuickMark> marks,
  ) async {
    savedRefs = List.unmodifiable(mediaRefs);
  }
}
