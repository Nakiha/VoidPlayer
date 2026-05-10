import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/action_registry.dart';
import 'package:void_player/actions/player_action.dart';
import 'package:void_player/analysis/analysis_cache.dart';
import 'package:void_player/analysis/analysis_manager.dart';
import 'package:void_player/analysis/analysis_overlay.dart';
import 'package:void_player/analysis/analysis_toolbar_data_source.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/widgets/analysis_overlay_controls.dart';
import 'package:void_player/widgets/media_header.dart';
import 'package:void_player/widgets/toolbar.dart';

class _FakeAnalysisToolbarDataSource extends ChangeNotifier
    implements AnalysisToolbarDataSource {
  final AnalysisCacheSnapshot cacheSnapshot;
  final Map<String, int> bytesByHash;
  final String? overlayHash;
  final Set<int> overlayTrackFileIds;
  AnalysisOverlayConfig config;

  _FakeAnalysisToolbarDataSource({
    this.cacheSnapshot = const AnalysisCacheSnapshot(
      path: '',
      totalBytes: 0,
      indexedBytes: 0,
      unindexedBytes: 0,
      maxBytes: 0,
      entries: [],
    ),
    this.bytesByHash = const {},
    this.overlayHash,
    Set<int>? overlayTrackFileIds,
    this.config = const AnalysisOverlayConfig(),
  }) : overlayTrackFileIds =
           overlayTrackFileIds ?? (overlayHash == null ? const {} : const {1});

  @override
  String? get activeOverlayHash => overlayHash;

  @override
  bool get overlayPanelVisible => overlayHash != null;

  @override
  Set<int> get activeOverlayTrackFileIds => overlayTrackFileIds;

  @override
  AnalysisOverlayConfig get overlayConfig => config;

  @override
  AnalysisState get state => AnalysisState.idle;

  @override
  AnalysisError? get error => null;

  @override
  AnalysisTrackGenerationStatus? statusForPath(String path) {
    for (final entry in cacheSnapshot.entries) {
      if (entry.videoPath == path) {
        return AnalysisTrackGenerationStatus(
          path: path,
          fileName: entry.name,
          hash: entry.hash,
          status: entry.complete
              ? AnalysisTrackStatus.cached
              : AnalysisTrackStatus.generating,
          progress: entry.complete ? 1 : 0,
          error: null,
        );
      }
    }
    if (overlayHash == 'hash1' && path == 'track.mp4') {
      return const AnalysisTrackGenerationStatus(
        path: 'track.mp4',
        fileName: 'track.mp4',
        hash: 'hash1',
        status: AnalysisTrackStatus.cached,
        progress: 1,
        error: null,
      );
    }
    return null;
  }

  @override
  Future<AnalysisCacheSnapshot> snapshot() => Future.value(cacheSnapshot);

  @override
  Future<Map<String, int>> currentBytesByHash(Set<String> hashes) =>
      Future.value(bytesByHash);

  @override
  String formatBytes(int bytes) => '$bytes B';
}

Widget _localized(Widget child) => MaterialApp(
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  home: Scaffold(body: child),
);

void main() {
  setUpAll(() async {
    await initLogging(const []);
  });

  TrackEntry track() => const TrackEntry(
    TrackInfo(fileId: 1, slot: 0, path: 'track.mp4', width: 1920, height: 1080),
  );

  Widget buildToolbar({
    required List<TrackEntry> tracks,
    required VoidCallback onProfiler,
    Future<void> Function()? onOpenFile,
    Future<void> Function(String url)? onOpenNetworkMedia,
    AnalysisToolbarDataSource? analysisDataSource,
    bool? analysisEnabled,
    ActionRegistry? actionRegistry,
    VoidCallback? onTogglePlay,
  }) {
    final registry = actionRegistry;
    if (registry != null && onTogglePlay != null) {
      registry.bind(const TogglePlayPause(), (_) => onTogglePlay());
    }
    final toolbar = Scaffold(
      body: AppToolBar(
        viewMode: 0,
        onViewModeChanged: (_) {},
        onOpenFile: onOpenFile ?? () async {},
        onOpenNetworkMedia: onOpenNetworkMedia ?? (_) async {},
        onOpenSshRemoteMedia: (_) async {},
        onMediaInfo: () {},
        onAnalysis: () async {},
        onProfiler: onProfiler,
        onSettings: () {},
        tracks: tracks,
        analysisDataSource:
            analysisDataSource ?? _FakeAnalysisToolbarDataSource(),
        analysisEnabled: analysisEnabled ?? tracks.isNotEmpty,
      ),
    );
    return MaterialApp(
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: registry == null
          ? toolbar
          : ActionFocus(actionRegistry: registry, child: toolbar),
    );
  }

  testWidgets('profiler button is disabled until tracks are loaded', (
    tester,
  ) async {
    var profilerTaps = 0;

    await tester.pumpWidget(
      buildToolbar(tracks: const [], onProfiler: () => profilerTaps++),
    );
    await tester.tap(find.widgetWithIcon(IconButton, Icons.speed));
    expect(profilerTaps, 0);

    await tester.pumpWidget(
      buildToolbar(tracks: [track()], onProfiler: () => profilerTaps++),
    );
    await tester.tap(find.widgetWithIcon(IconButton, Icons.speed));
    expect(profilerTaps, 1);
  });

  testWidgets('add media main button opens the file picker action', (
    tester,
  ) async {
    var openFileTaps = 0;

    await tester.pumpWidget(
      buildToolbar(
        tracks: const [],
        onProfiler: () {},
        onOpenFile: () async => openFileTaps++,
      ),
    );

    await tester.tap(find.text('Add Media'));
    expect(openFileTaps, 1);
  });

  testWidgets('media header toggles overlay panel from first track only', (
    tester,
  ) async {
    var panelTaps = 0;
    final openedTrack = track();
    final secondTrack = const TrackEntry(
      TrackInfo(
        fileId: 2,
        slot: 1,
        path: 'second.mp4',
        width: 1920,
        height: 1080,
      ),
    );

    await tester.pumpWidget(
      _localized(
        MediaHeaderBar(
          entries: [openedTrack, secondTrack],
          analysisDataSource: _FakeAnalysisToolbarDataSource(),
          onMediaSwapped: (_, _) {},
          onAnalysisOverlayPanelToggle: () async => panelTaps++,
          onAnalysisOverlayTypeChanged: (_) {},
          onAnalysisOverlayLayersChanged: (_) {},
          onAnalysisOverlayOpacityChanged: (_) {},
          onRemoveClicked: (_) {},
        ),
      ),
    );

    await tester.pump();
    expect(find.widgetWithIcon(IconButton, Icons.grid_on), findsOneWidget);
    await tester.tap(find.widgetWithIcon(IconButton, Icons.grid_on));

    expect(panelTaps, 1);
  });

  testWidgets('analysis panel reports cache without overlay controls', (
    tester,
  ) async {
    final openedTrack = track();

    await tester.pumpWidget(
      buildToolbar(
        tracks: [openedTrack],
        onProfiler: () {},
        analysisDataSource: _FakeAnalysisToolbarDataSource(
          cacheSnapshot: AnalysisCacheSnapshot(
            path: '',
            totalBytes: 4096,
            indexedBytes: 4096,
            unindexedBytes: 0,
            maxBytes: 0,
            entries: [
              AnalysisCacheEntryStats(
                hash: 'hash1',
                name: openedTrack.fileName,
                videoPath: openedTrack.path,
                videoBytes: 1024,
                analysisBytes: 4096,
                cachedAt: null,
                lastAccessedAt: null,
                complete: true,
              ),
            ],
          ),
          bytesByHash: const {'hash1': 4096},
        ),
      ),
    );

    await tester.tap(find.widgetWithIcon(IconButton, Icons.analytics_outlined));
    await tester.pumpAndSettle();
    expect(find.text(openedTrack.fileName), findsOneWidget);
    expect(find.widgetWithIcon(IconButton, Icons.grid_on), findsNothing);
  });

  testWidgets('analysis overlay control bar switches type and opacity', (
    tester,
  ) async {
    final openedTrack = track();
    var type = AnalysisOverlayType.cu;
    var opacity = 0.55;
    final source = _FakeAnalysisToolbarDataSource(
      overlayHash: 'hash1',
      config: const AnalysisOverlayConfig(),
    );

    source.config = source.config.copyWith(opacity: opacity);

    await tester.pumpWidget(
      _localized(
        AnalysisOverlayControlBar(
          entries: [openedTrack],
          dataSource: source,
          onTypeChanged: (next) => type = next,
          onLayersChanged: (_) {},
          onOpacityChanged: (next) => opacity = next,
        ),
      ),
    );

    expect(find.byKey(analysisOverlayControlBarKey), findsOneWidget);
    await tester.tap(
      find.byKey(
        ValueKey(
          'analysis-overlay-type-${openedTrack.fileId}-${AnalysisOverlayType.qpHeatmap.name}',
        ),
      ),
    );
    expect(type, AnalysisOverlayType.qpHeatmap);

    final slider = find.byKey(
      ValueKey('analysis-overlay-opacity-${openedTrack.fileId}'),
    );
    await tester.drag(slider, const Offset(80, 0));
    expect(opacity, greaterThan(0.55));
  });

  testWidgets('analysis overlay sync disables inactive track panels by default', (
    tester,
  ) async {
    final openedTrack = track();
    const secondTrack = TrackEntry(
      TrackInfo(
        fileId: 2,
        slot: 1,
        path: 'second.mp4',
        width: 1920,
        height: 1080,
      ),
    );
    var type = AnalysisOverlayType.cu;
    final source = _FakeAnalysisToolbarDataSource(
      overlayHash: 'hash1',
      overlayTrackFileIds: const {1, 2},
      cacheSnapshot: const AnalysisCacheSnapshot(
        path: '',
        totalBytes: 0,
        indexedBytes: 0,
        unindexedBytes: 0,
        maxBytes: 0,
        entries: [
          AnalysisCacheEntryStats(
            hash: 'hash1',
            name: 'track.mp4',
            videoPath: 'track.mp4',
            videoBytes: 1,
            analysisBytes: 1,
            cachedAt: null,
            lastAccessedAt: null,
            complete: true,
          ),
          AnalysisCacheEntryStats(
            hash: 'hash2',
            name: 'second.mp4',
            videoPath: 'second.mp4',
            videoBytes: 1,
            analysisBytes: 1,
            cachedAt: null,
            lastAccessedAt: null,
            complete: true,
          ),
        ],
      ),
    );

    await tester.pumpWidget(
      _localized(
        AnalysisOverlayControlBar(
          entries: [openedTrack, secondTrack],
          dataSource: source,
          onTypeChanged: (next) => type = next,
          onLayersChanged: (_) {},
          onOpacityChanged: (_) {},
        ),
      ),
    );

    final secondTrackQpButton = find.byKey(
      ValueKey(
        'analysis-overlay-type-${secondTrack.fileId}-${AnalysisOverlayType.qpHeatmap.name}',
      ),
    );
    await tester.tap(secondTrackQpButton, warnIfMissed: false);
    expect(type, AnalysisOverlayType.cu);

    await tester.tap(
      find.byKey(ValueKey('analysis-overlay-sync-${openedTrack.fileId}')),
    );
    await tester.pump();
    await tester.tap(secondTrackQpButton);
    expect(type, AnalysisOverlayType.qpHeatmap);
  });

  testWidgets('canceling network stream dialog restores space shortcut', (
    tester,
  ) async {
    final actionRegistry = ActionRegistry();
    var togglePlayTaps = 0;

    await tester.pumpWidget(
      buildToolbar(
        tracks: const [],
        onProfiler: () {},
        actionRegistry: actionRegistry,
        onTogglePlay: () => togglePlayTaps++,
      ),
    );

    await tester.tap(find.byIcon(Icons.arrow_drop_down));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Open network stream...'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Cancel'));
    await tester.pumpAndSettle();

    await tester.sendKeyDownEvent(LogicalKeyboardKey.space);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.space);
    await tester.pump();

    expect(togglePlayTaps, 1);
    expect(find.text('Open network stream...'), findsNothing);
  });
}
