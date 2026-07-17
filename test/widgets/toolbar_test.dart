import 'dart:ui' show SemanticsAction;

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
import 'package:void_player/feedback/app_feedback.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/widgets/analysis_overlay_controls.dart';
import 'package:void_player/widgets/controls_bar.dart';
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
  bool supportsOverlayForHash(String hash) => true;

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
    bool canAddTrack = true,
    bool canRunAnalysis = true,
    bool localFilePlaybackAvailable = true,
    bool networkMediaAvailable = true,
    bool sshRemoteMediaAvailable = true,
    bool nativeFilePickerAvailable = true,
    String? addMediaDisabledTooltip,
    String? analysisDisabledTooltip,
    ActionRegistry? actionRegistry,
    VoidCallback? onTogglePlay,
    VoidCallback? onMarksSidebarToggle,
    bool marksSidebarActive = false,
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
        onMarksSidebarToggle: onMarksSidebarToggle ?? () {},
        tracks: tracks,
        analysisDataSource:
            analysisDataSource ?? _FakeAnalysisToolbarDataSource(),
        localFilePlaybackAvailable: localFilePlaybackAvailable,
        networkMediaAvailable: networkMediaAvailable,
        sshRemoteMediaAvailable: sshRemoteMediaAvailable,
        nativeFilePickerAvailable: nativeFilePickerAvailable,
        addMediaDisabledTooltip: addMediaDisabledTooltip,
        analysisDisabledTooltip: analysisDisabledTooltip,
        canAddTrack: canAddTrack,
        canRunAnalysis: canRunAnalysis,
        analysisEnabled: analysisEnabled ?? tracks.isNotEmpty,
        marksSidebarActive: marksSidebarActive,
      ),
    );
    return MaterialApp(
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: AppFeedbackScope(
        controller: AppFeedbackController(),
        child: registry == null
            ? toolbar
            : ActionFocus(actionRegistry: registry, child: toolbar),
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

  testWidgets('add media disabled state exposes platform detail tooltip', (
    tester,
  ) async {
    const detail = 'macOS phase 1 does not enable network media playback.';

    await tester.pumpWidget(
      buildToolbar(
        tracks: const [],
        onProfiler: () {},
        canAddTrack: false,
        localFilePlaybackAvailable: false,
        networkMediaAvailable: false,
        sshRemoteMediaAvailable: false,
        nativeFilePickerAvailable: false,
        addMediaDisabledTooltip: detail,
      ),
    );

    expect(find.byTooltip(detail), findsAtLeastNWidgets(1));
  });

  testWidgets('analysis disabled state exposes platform detail tooltip', (
    tester,
  ) async {
    const detail = 'macOS analysis UI/IPC remains capability-gated.';

    await tester.pumpWidget(
      buildToolbar(
        tracks: [track()],
        onProfiler: () {},
        canRunAnalysis: false,
        analysisEnabled: false,
        analysisDisabledTooltip: detail,
      ),
    );

    expect(find.byTooltip(detail), findsOneWidget);
  });

  testWidgets('analysis hover panel refreshes after track updates', (
    tester,
  ) async {
    final firstTrack = track();
    const secondTrack = TrackEntry(
      TrackInfo(
        fileId: 2,
        slot: 1,
        path: 'second.mp4',
        width: 1920,
        height: 1080,
      ),
    );

    await tester.pumpWidget(
      buildToolbar(tracks: [firstTrack], onProfiler: () {}),
    );
    await tester.tap(find.widgetWithIcon(IconButton, Icons.analytics_outlined));
    await tester.pump();
    expect(tester.takeException(), isNull);

    await tester.pumpWidget(
      buildToolbar(tracks: [firstTrack, secondTrack], onProfiler: () {}),
    );
    expect(tester.takeException(), isNull);
    await tester.pump();
    expect(tester.takeException(), isNull);
  });

  testWidgets('marks sidebar toggle invokes the toolbar action', (
    tester,
  ) async {
    var toggleTaps = 0;

    await tester.pumpWidget(
      buildToolbar(
        tracks: [track()],
        onProfiler: () {},
        onMarksSidebarToggle: () => toggleTaps++,
      ),
    );

    await tester.tap(find.byTooltip('Marks sidebar'));
    expect(toggleTaps, 1);
  });

  testWidgets('media header click toggles overlay controls request', (
    tester,
  ) async {
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
    var toggleTaps = 0;

    await tester.pumpWidget(
      _localized(
        MediaHeaderBar(
          entries: [openedTrack, secondTrack],
          analysisDataSource: _FakeAnalysisToolbarDataSource(),
          onAnalysisOverlayControlsToggle: () => toggleTaps++,
          onMediaSwapped: (_, _) {},
          onRemoveClicked: (_) {},
        ),
      ),
    );

    await tester.pump();
    expect(find.widgetWithIcon(IconButton, Icons.grid_on), findsOneWidget);

    await tester.tap(find.widgetWithIcon(IconButton, Icons.grid_on));
    await tester.pump();

    expect(toggleTaps, 1);
    expect(find.byKey(analysisOverlayControlBarKey), findsNothing);
  });

  testWidgets('media header overlay target remains a named AXTree button', (
    tester,
  ) async {
    final semantics = tester.ensureSemantics();

    await tester.pumpWidget(
      _localized(
        MediaHeaderBar(
          entries: [track()],
          analysisDataSource: _FakeAnalysisToolbarDataSource(),
          onAnalysisOverlayControlsToggle: () {},
          onMediaSwapped: (_, _) {},
          onRemoveClicked: (_) {},
        ),
      ),
    );

    final target = find.bySemanticsLabel('Show bitstream overlay controls');
    expect(target, findsOneWidget);
    expect(
      tester
          .getSemantics(target)
          .getSemanticsData()
          .hasAction(SemanticsAction.tap),
      isTrue,
    );
    semantics.dispose();
  });

  testWidgets('playback controls expose stable named actions and seek slider', (
    tester,
  ) async {
    final semantics = tester.ensureSemantics();

    await tester.pumpWidget(
      _localized(
        SizedBox(
          width: 1000,
          child: ControlsBar(
            timelineStartWidth: 500,
            zoomRatio: 1,
            onZoomChanged: (_) {},
            isPlaying: false,
            isFullScreen: false,
            onTogglePlay: () async {},
            onToggleFullScreen: () {},
            onStepForward: () async {},
            onStepBackward: () async {},
            currentPtsUs: 1000000,
            durationUs: 9000000,
            onSeek: (_) {},
          ),
        ),
      ),
    );

    expect(find.bySemanticsLabel('Playback controls'), findsOneWidget);
    expect(find.bySemanticsLabel('Zoom'), findsOneWidget);
    for (final label in const [
      'Enter Full Screen',
      'Previous Frame',
      'Play',
      'Next Frame',
    ]) {
      final target = find.bySemanticsLabel(label);
      expect(target, findsOneWidget, reason: label);
      expect(
        tester
            .getSemantics(target)
            .getSemanticsData()
            .hasAction(SemanticsAction.tap),
        isTrue,
        reason: label,
      );
    }

    final timeline = find.bySemanticsLabel('Timeline seek');
    expect(timeline, findsOneWidget);
    final timelineData = tester.getSemantics(timeline).getSemanticsData();
    expect(timelineData.hasAction(SemanticsAction.increase), isTrue);
    expect(timelineData.hasAction(SemanticsAction.decrease), isTrue);
    semantics.dispose();
  });

  testWidgets('analysis overlay strip close hides controls only', (
    tester,
  ) async {
    final openedTrack = track();
    var closeTaps = 0;
    var deactivateTaps = 0;

    await tester.pumpWidget(
      _localized(
        AnalysisOverlayStrip(
          entries: [openedTrack],
          dataSource: _FakeAnalysisToolbarDataSource(overlayHash: 'hash1'),
          visible: true,
          onTypeChanged: (_) {},
          onOpacityChanged: (_) {},
          onActivateOverlay: () async {},
          onDeactivateOverlay: () => deactivateTaps++,
          onClose: () => closeTaps++,
        ),
      ),
    );

    await tester.pump();
    expect(find.byKey(analysisOverlayStripKey), findsOneWidget);

    await tester.tap(find.byTooltip('Close'));
    expect(closeTaps, 1);
    expect(deactivateTaps, 0);
  });

  testWidgets('media header overlay button is only on the first track', (
    tester,
  ) async {
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
          onRemoveClicked: (_) {},
        ),
      ),
    );

    await tester.pump();
    expect(find.widgetWithIcon(IconButton, Icons.grid_on), findsOneWidget);
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
    final semantics = tester.ensureSemantics();
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
          dataSource: source,
          onTypeChanged: (next) => type = next,
          onOpacityChanged: (next) => opacity = next,
        ),
      ),
    );

    expect(find.byKey(analysisOverlayControlBarKey), findsOneWidget);
    final overlayTooltips = find.descendant(
      of: find.byKey(analysisOverlayControlBarKey),
      matching: find.byType(Tooltip),
    );
    final tooltipMessages = tester
        .widgetList<Tooltip>(overlayTooltips)
        .map((tooltip) => tooltip.message)
        .toSet();
    expect(
      tooltipMessages,
      containsAll(const [
        'CU partitions',
        'QP heatmap',
        'Bit-cost heatmap',
        'Overlay opacity',
      ]),
    );
    for (final tooltip in tester.widgetList<Tooltip>(overlayTooltips)) {
      expect(tooltip.excludeFromSemantics, isTrue);
    }

    for (final label in const [
      'CU partitions',
      'QP heatmap',
      'Bit-cost heatmap',
    ]) {
      final target = find.bySemanticsLabel(label);
      expect(target, findsOneWidget, reason: label);
      expect(
        tester
            .getSemantics(target)
            .getSemanticsData()
            .hasAction(SemanticsAction.tap),
        isTrue,
        reason: label,
      );
    }
    final opacityNode = find.bySemanticsLabel('Overlay opacity');
    expect(opacityNode, findsOneWidget);
    final opacityData = tester.getSemantics(opacityNode).getSemanticsData();
    expect(opacityData.value, '55%');
    expect(opacityData.hasAction(SemanticsAction.increase), isTrue);
    expect(opacityData.hasAction(SemanticsAction.decrease), isTrue);

    await tester.tap(
      find.byKey(
        ValueKey('analysis-overlay-type-${AnalysisOverlayType.qpHeatmap.name}'),
      ),
    );
    expect(type, AnalysisOverlayType.qpHeatmap);

    final slider = find.byKey(analysisOverlayOpacityKey);
    await tester.tapAt(tester.getTopRight(slider) + const Offset(-2, 12));
    expect(opacity, greaterThan(0.55));
    await tester.tapAt(tester.getTopLeft(slider) + const Offset(0, 12));
    expect(opacity, lessThanOrEqualTo(0.01));

    expect(source.config.copyWith(opacity: -1).opacity, 0);
    expect(source.config.copyWith(opacity: 2).opacity, 1);
    semantics.dispose();
  });

  testWidgets('analysis overlay type segments toggle activation', (
    tester,
  ) async {
    var type = AnalysisOverlayType.cu;
    var activations = 0;
    var deactivations = 0;
    final source = _FakeAnalysisToolbarDataSource(
      overlayHash: 'hash1',
      config: const AnalysisOverlayConfig(),
    );

    await tester.pumpWidget(
      _localized(
        AnalysisOverlayControlBar(
          dataSource: source,
          onTypeChanged: (next) => type = next,
          onOpacityChanged: (_) {},
          onActivateOverlay: () async => activations++,
          onDeactivateOverlay: () => deactivations++,
        ),
      ),
    );

    await tester.tap(
      find.byKey(
        ValueKey('analysis-overlay-type-${AnalysisOverlayType.cu.name}'),
      ),
    );
    expect(deactivations, 1);

    await tester.pumpWidget(
      _localized(
        AnalysisOverlayControlBar(
          dataSource: _FakeAnalysisToolbarDataSource(),
          panelActive: false,
          panelReady: true,
          onTypeChanged: (next) => type = next,
          onOpacityChanged: (_) {},
          onActivateOverlay: () async => activations++,
          onDeactivateOverlay: () => deactivations++,
        ),
      ),
    );

    await tester.tap(
      find.byKey(
        ValueKey('analysis-overlay-type-${AnalysisOverlayType.qpHeatmap.name}'),
      ),
    );
    expect(type, AnalysisOverlayType.qpHeatmap);
    expect(activations, 1);
  });

  testWidgets(
    'analysis overlay controls remain clickable before cache is ready',
    (tester) async {
      var type = AnalysisOverlayType.cu;
      var activations = 0;

      await tester.pumpWidget(
        _localized(
          AnalysisOverlayControlBar(
            dataSource: _FakeAnalysisToolbarDataSource(),
            panelActive: false,
            panelReady: false,
            onTypeChanged: (next) => type = next,
            onOpacityChanged: (_) {},
            onActivateOverlay: () async => activations++,
            onDeactivateOverlay: () {},
          ),
        ),
      );

      await tester.tap(
        find.byKey(
          ValueKey(
            'analysis-overlay-type-${AnalysisOverlayType.cuBitCostHeatmap.name}',
          ),
        ),
      );

      expect(type, AnalysisOverlayType.cuBitCostHeatmap);
      expect(activations, 1);
    },
  );

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
