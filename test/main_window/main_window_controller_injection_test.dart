import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/action_registry.dart';
import 'package:void_player/analysis/analysis_cache.dart';
import 'package:void_player/analysis/analysis_manager.dart';
import 'package:void_player/analysis/analysis_overlay.dart';
import 'package:void_player/analysis/analysis_toolbar_data_source.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/config/app_settings_repository.dart';
import 'package:void_player/main_window/main_window_controller.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/marks/quick_mark_persistence.dart';
import 'package:void_player/marks/quick_mark_thumbnail.dart';
import 'package:void_player/platform/analysis_process_host.dart';
import 'package:void_player/platform/main_window_platform.dart';
import 'package:void_player/platform/platform_capabilities.dart';
import 'package:void_player/preferences/playback_preferences.dart';
import 'package:void_player/startup_options.dart';
import 'package:void_player/video_renderer_controller.dart';

class _FakeMainWindowPlatform implements MainWindowPlatform {
  _FakeMainWindowPlatform({this.onSetFullScreen});

  final List<bool> fullScreenCalls = [];
  final void Function(bool fullScreen)? onSetFullScreen;

  @override
  Future<Rect> getBounds() =>
      Future.value(const Rect.fromLTWH(0, 0, 1280, 720));

  @override
  Future<void> setFullScreen(bool fullScreen) async {
    onSetFullScreen?.call(fullScreen);
    fullScreenCalls.add(fullScreen);
  }
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
  Map<String, Object?> nativeOverlayStatePayload() => const {};

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
  int markThumbnailCacheMaxBytes = 0;

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

Future<void> _waitForTestCondition(
  bool Function() predicate,
  String description, {
  Duration timeout = const Duration(seconds: 1),
}) async {
  final stopwatch = Stopwatch()..start();
  while (!predicate()) {
    if (stopwatch.elapsed >= timeout) {
      fail('Timed out waiting for $description');
    }
    await Future<void>.delayed(const Duration(milliseconds: 1));
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUpAll(() async {
    await initLogging(['--log-level=flutter=OFF']);
  });

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
      expect(
        controller.viewModel.media.externalAnalysisWindowsCapability.detail,
        contains('analysis UI/IPC'),
      );
      expect(
        controller.viewModel.media.networkMediaPlaybackCapability.detail,
        contains('network media playback'),
      );
    },
  );

  testWidgets(
    'fullscreen toggle delegates to platform and updates view model',
    (tester) async {
      final platformObservedStates = <bool>[];
      late final MainWindowController controller;
      final platformWindow = _FakeMainWindowPlatform(
        onSetFullScreen: (_) {
          platformObservedStates.add(controller.viewModel.overlays.fullScreen);
        },
      );
      controller = MainWindowController(
        actionRegistry: ActionRegistry(),
        vsync: const TestVSync(),
        startupOptions: const StartupOptions(),
        mounted: () => true,
        platformWindow: platformWindow,
        analysisGeneration: _FakeAnalysisGenerationService(),
        analysisToolbarDataSource: _FakeAnalysisToolbarDataSource(),
        appSettings: _FakeAppSettingsRepository(),
        playbackPreferences: _FakePlaybackPreferences(),
      );
      addTearDown(controller.dispose);

      controller.viewActions.mediaTimeline.onToggleFullScreen();
      await tester.pump();
      await tester.pump();

      expect(platformWindow.fullScreenCalls, const [true]);
      expect(platformObservedStates, const [true]);
      expect(controller.viewModel.overlays.fullScreen, isTrue);
      expect(controller.viewModel.overlays.fullScreenControlsVisible, isTrue);

      controller.fullScreenCoordinator.dispose();
      await tester.pump();
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

  test('media timeline offset failure reports user action error', () async {
    const channel = MethodChannel('video_renderer');
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          switch (call.method) {
            case 'createPlayer':
              return {'textureId': 1, 'tracks': const <Map<String, Object?>>[]};
            case 'setTrackOffset':
              throw PlatformException(
                code: 'native-error',
                message: 'offset failed',
              );
            default:
              return null;
          }
        });
    addTearDown(() {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null);
    });

    final failures = <String>[];
    final controller = MainWindowController(
      actionRegistry: ActionRegistry(),
      vsync: const TestVSync(),
      startupOptions: const StartupOptions(),
      mounted: () => true,
      analysisGeneration: _FakeAnalysisGenerationService(),
      analysisToolbarDataSource: _FakeAnalysisToolbarDataSource(),
      appSettings: _FakeAppSettingsRepository(),
      playbackPreferences: _FakePlaybackPreferences(),
      onUserActionFailed: (operation, error) {
        failures.add('$operation: $error');
      },
    );
    addTearDown(controller.dispose);
    await controller.player.createPlayer(const ['/tmp/video.mp4']);
    controller.trackManager.addTrack(
      const TrackInfo(
        fileId: 1,
        slot: 0,
        path: '/tmp/video.mp4',
        width: 320,
        height: 180,
      ),
    );

    await controller.viewActions.mediaTimeline.onOffsetChanged(1, 10);

    expect(failures, hasLength(1));
    expect(failures.single, contains('adjust track offset'));
    expect(failures.single, contains('offset failed'));
  });

  test('quick mark jump previews target frame before selecting the mark', () {
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
            ptsUs: 5000,
            dtsUs: 4900,
            sourcePacketIndex: 8,
            sourcePacketSize: 1024,
          ),
          sourceRect: Rect.fromLTRB(0.3, 0.3, 0.4, 0.4),
        ),
      ])
      ..setPolledPlaybackState(
        1000,
        10000,
        false,
        presentedFrameAnchors: const {
          1: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
        },
      );

    expect(
      controller.viewModel.viewport.quickMarks.map((mark) => mark.id),
      const [1],
    );

    controller.viewActions.marks.onJumpToMark(2);

    expect(controller.stateStore.value.currentPtsUs, 5000);
    expect(controller.stateStore.value.pendingSeekUs, 5000);
    var viewModel = controller.viewModel;
    expect(viewModel.viewport.quickMarks, isEmpty);
    expect(viewModel.viewport.selectedQuickMarkId, isNull);

    controller.stateStore
      ..setPendingSeek(null, null)
      ..setPolledPlaybackState(
        5000,
        10000,
        false,
        presentedFrameAnchors: const {
          1: QuickMarkAnchor(
            fileId: 1,
            ptsUs: 5000,
            dtsUs: 4900,
            sourcePacketIndex: 8,
            sourcePacketSize: 1024,
          ),
        },
      );

    viewModel = controller.viewModel;
    expect(viewModel.viewport.quickMarks.map((mark) => mark.id), const [2]);
    expect(viewModel.viewport.selectedQuickMarkId, 2);
  });

  test(
    'quick mark jump focuses visible mark and supersedes pending seek',
    () async {
      const channel = MethodChannel('video_renderer');
      final seekCalls = <Map<dynamic, dynamic>>[];
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (call) async {
            switch (call.method) {
              case 'createPlayer':
                return {
                  'textureId': 1,
                  'tracks': const <Map<String, Object?>>[],
                };
              case 'seek':
                seekCalls.add(
                  Map<dynamic, dynamic>.from(call.arguments as Map),
                );
                return null;
              default:
                return null;
            }
          });
      addTearDown(() {
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(channel, null);
      });

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
      await controller.player.createPlayer(const ['/tmp/video.mp4']);
      controller.trackManager.addTrack(
        const TrackInfo(
          fileId: 1,
          slot: 0,
          path: '/tmp/video.mp4',
          width: 320,
          height: 180,
        ),
      );
      controller.viewActions.viewport.onResize(320, 180, 1.0);
      controller.stateStore
        ..setQuickMarks(const [
          QuickMark(
            id: 1,
            anchor: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
            sourceRect: Rect.fromLTRB(0.1, 0.1, 0.2, 0.2),
          ),
        ])
        ..setPolledPlaybackState(
          1000,
          10000,
          false,
          presentedFrameAnchors: const {
            1: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
          },
        )
        ..setPendingSeek(5000, DateTime.now());

      final previousLayout = controller.stateStore.value.layout;

      controller.viewActions.marks.onJumpToMark(1);
      await Future<void>.delayed(Duration.zero);

      final nextLayout = controller.stateStore.value.layout;
      expect(controller.viewModel.viewport.selectedQuickMarkId, 1);
      expect(nextLayout.zoomRatio, isNot(previousLayout.zoomRatio));
      expect(seekCalls, hasLength(1));
      expect(seekCalls.single['ptsUs'], 1000);
      expect(seekCalls.single['requestId'], isA<int>());
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

      await _waitForTestCondition(
        () =>
            repository.loadedRefs.isNotEmpty &&
            controller.viewModel.marks.allMarks.isNotEmpty,
        'runtime quick marks to load',
      );

      expect(repository.loadedRefs.single.fileId, 4);
      expect(repository.savedRefs, isEmpty);
      expect(controller.viewModel.marks.allMarks, hasLength(1));
      expect(controller.viewModel.marks.allMarks.single.fileId, 4);
      expect(controller.viewModel.marks.allMarks.single.text, 'persisted');
      expect(
        controller.viewModel.marks.thumbnailsByMarkId[8]?.status,
        QuickMarkThumbnailStatus.queued,
      );
    },
  );

  test('starting a quick mark pauses playback immediately', () async {
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
      ..setTextureId(1)
      ..setPolledPlaybackState(1000, 10000, true);
    controller.viewActions.viewport.onResize(320, 180, 1.0);

    controller.viewActions.viewport.onQuickMarkStart(const Offset(160, 90));
    await Future<void>.delayed(Duration.zero);

    expect(controller.viewModel.playback.isPlaying, isFalse);
    expect(controller.viewModel.viewport.quickMarkDraft, isNotNull);
  });

  test(
    'arrow quick mark thumbnail capture is centered on arrow head',
    () async {
      final channel = const MethodChannel('video_renderer');
      final captureArgs = <Map<dynamic, dynamic>>[];
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (call) async {
            switch (call.method) {
              case 'createPlayer':
                return {
                  'textureId': 1,
                  'tracks': const <Map<String, Object?>>[],
                };
              case 'currentPts':
                return 0;
              case 'duration':
                return 10000;
              case 'isPlaying':
                return false;
              case 'currentPresentedFrame':
                return {'ptsUs': 0, 'dtsUs': 0};
              case 'captureViewportRegion':
                final args = Map<dynamic, dynamic>.from(call.arguments as Map);
                captureArgs.add(args);
                return {
                  'hash': 'region',
                  'width': 100,
                  'height': 80,
                  'avgLuma': 1.0,
                  'nonBlackRatio': 1.0,
                  'outputPath': args['outputPath'],
                };
              default:
                return null;
            }
          });
      addTearDown(() {
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(channel, null);
      });

      final repository = _FakeQuickMarkRepository(
        loadedMarks: const [
          QuickMark(
            id: 21,
            anchor: QuickMarkAnchor(fileId: 99, ptsUs: 0, dtsUs: 0),
            sourceRect: Rect.fromLTRB(0.2, 0.2, 0.7, 0.6),
            sourceStart: Offset(0.2, 0.2),
            sourceEnd: Offset(0.7, 0.6),
            shape: QuickMarkShape.arrow,
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
      await controller.player.createPlayer(['a.mp4'], width: 400, height: 200);
      controller.stateStore.setTextureId(1);
      controller.viewActions.viewport.onResize(400, 200, 1.0);
      controller.start();
      controller.trackManager.addTrack(
        const TrackInfo(
          fileId: 4,
          slot: 0,
          path: '/tmp/video.mp4',
          width: 100,
          height: 100,
        ),
      );

      await Future<void>.delayed(const Duration(milliseconds: 260));

      expect(captureArgs, isNotEmpty);
      final args = captureArgs.single;
      expect(args['x'], 190);
      expect(args['y'], 80);
      expect(args['width'], 100);
      expect(args['height'], 80);
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
