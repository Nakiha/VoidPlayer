import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/action_registry.dart';
import 'package:void_player/actions/player_action.dart';
import 'package:void_player/analysis/analysis_cache.dart';
import 'package:void_player/analysis/analysis_manager.dart';
import 'package:void_player/analysis/analysis_toolbar_data_source.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/widgets/toolbar.dart';

class _FakeAnalysisToolbarDataSource extends ChangeNotifier
    implements AnalysisToolbarDataSource {
  final AnalysisCacheSnapshot cacheSnapshot;
  final Map<String, int> bytesByHash;

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
  });

  @override
  String? get activeOverlayHash => null;

  @override
  AnalysisState get state => AnalysisState.idle;

  @override
  AnalysisError? get error => null;

  @override
  AnalysisTrackGenerationStatus? statusForPath(String path) => null;

  @override
  Future<AnalysisCacheSnapshot> snapshot() => Future.value(cacheSnapshot);

  @override
  Future<Map<String, int>> currentBytesByHash(Set<String> hashes) =>
      Future.value(bytesByHash);

  @override
  String formatBytes(int bytes) => '$bytes B';
}

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
    Future<void> Function(TrackEntry track, String hash)? onOverlayToggle,
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
        onAnalysisOverlayToggle: onOverlayToggle ?? (_, _) async {},
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

  testWidgets('analysis panel enables overlay only for cached tracks', (
    tester,
  ) async {
    var overlayTaps = 0;
    String? overlayHash;
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
        onOverlayToggle: (track, hash) async {
          overlayTaps++;
          overlayHash = hash;
        },
      ),
    );

    await tester.tap(find.widgetWithIcon(IconButton, Icons.analytics_outlined));
    await tester.pumpAndSettle();
    await tester.tap(find.widgetWithIcon(IconButton, Icons.grid_on));

    expect(overlayTaps, 1);
    expect(overlayHash, 'hash1');
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
