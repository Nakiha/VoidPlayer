import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_cache.dart';
import 'package:void_player/analysis/analysis_manager.dart';
import 'package:void_player/analysis/analysis_toolbar_data_source.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/widgets/toolbar.dart';

class _FakeAnalysisToolbarDataSource extends ChangeNotifier
    implements AnalysisToolbarDataSource {
  @override
  AnalysisState get state => AnalysisState.idle;

  @override
  AnalysisError? get error => null;

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
  TrackEntry track() => const TrackEntry(
    TrackInfo(fileId: 1, slot: 0, path: 'track.mp4', width: 1920, height: 1080),
  );

  Widget buildToolbar({
    required List<TrackEntry> tracks,
    required VoidCallback onProfiler,
    Future<void> Function()? onOpenFile,
  }) {
    return MaterialApp(
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: Scaffold(
        body: AppToolBar(
          viewMode: 0,
          onViewModeChanged: (_) {},
          onOpenFile: onOpenFile ?? () async {},
          onOpenNetworkMedia: (_) async {},
          onMediaInfo: () {},
          onAnalysis: () async {},
          onProfiler: onProfiler,
          onSettings: () {},
          tracks: tracks,
          analysisDataSource: _FakeAnalysisToolbarDataSource(),
        ),
      ),
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
}
