import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_cache.dart';
import 'package:void_player/analysis/analysis_ffi.dart';
import 'package:void_player/analysis/analysis_manager.dart';
import 'package:void_player/analysis/analysis_overlay.dart';
import 'package:void_player/analysis/analysis_quality_service.dart';
import 'package:void_player/analysis/analysis_toolbar_data_source.dart';
import 'package:void_player/analysis/nalu_types.dart';
import 'package:void_player/analysis/ui/analysis_ui_selection.dart';
import 'package:void_player/analysis/ui/page/analysis_page_view.dart';
import 'package:void_player/analysis/ui/testing/analysis_test_host.dart';
import 'package:void_player/analysis/ui/workspace/analysis_workspace_models.dart';
import 'package:void_player/analysis/ui/workspace/analysis_workspace_page.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/feedback/app_feedback.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/main_window/main_window_analysis_dock.dart';
import 'package:void_player/main_window/main_window_deck.dart';
import 'package:void_player/main_window/main_window_inspector.dart';
import 'package:void_player/main_window/main_window_list_sidebar.dart';
import 'package:void_player/main_window/main_window_media_sections.dart';
import 'package:void_player/main_window/main_window_overlays.dart';
import 'package:void_player/main_window/main_window_quality.dart';
import 'package:void_player/main_window/main_window_scaffold.dart';
import 'package:void_player/main_window/main_window_selection.dart';
import 'package:void_player/main_window/main_window_state.dart';
import 'package:void_player/main_window/main_window_view_handles.dart';
import 'package:void_player/main_window/main_window_view_model.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/platform/platform_capabilities.dart';
import 'package:void_player/preferences/playback_preferences.dart';
import 'package:void_player/session/playback_session.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/viewport/display_geometry.dart';
import 'package:void_player/viewport/viewport_display_state.dart';
import 'package:void_player/widgets/analysis_overlay_controls.dart';
import 'package:void_player/widgets/loop_range_bar.dart';
import 'package:void_player/widgets/quick_mark_sidebar.dart';
import 'package:void_player/widgets/timeline_area.dart';
import 'package:void_player/widgets/viewport_panel.dart';

void main() {
  setUpAll(() async {
    await initLogging(['--log-level=flutter=OFF']);
  });

  testWidgets('settings overlay is layered above main content', (tester) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(settingsVisible: false),
            handles: _handles(),
            actions: _noop,
          ),
        ),
      ),
    );

    final rootStack = tester.widget<Stack>(
      find
          .descendant(of: find.byType(Scaffold), matching: find.byType(Stack))
          .first,
    );
    final contentIndex = rootStack.children.indexWhere(
      (child) => child is Column,
    );
    final settingsIndex = rootStack.children.indexWhere(
      (child) => child is SettingsOverlaySlot,
    );

    expect(contentIndex, isNonNegative);
    expect(settingsIndex, isNonNegative);
    expect(settingsIndex, greaterThan(contentIndex));
  });

  testWidgets(
    'analysis overlay strip stays below viewport beside marks sidebar',
    (tester) async {
      final feedback = AppFeedbackController();
      addTearDown(feedback.dispose);
      final handles = _handles();
      final mediaTrack = const TrackEntry(
        TrackInfo(
          fileId: 1,
          slot: 0,
          path: 'track.mp4',
          width: 1920,
          height: 1080,
        ),
      );

      await tester.pumpWidget(
        _localized(
          AppFeedbackScope(
            controller: feedback,
            child: MainWindowScaffold(
              model: _model(
                settingsVisible: false,
                marksSidebarVisible: true,
                analysisOverlayControlsVisible: true,
                tracks: [mediaTrack],
              ),
              handles: handles,
              actions: _noop,
            ),
          ),
        ),
      );

      final viewportRect = tester.getRect(find.byType(ViewportPanel));
      final chromeRect = tester.getRect(find.byType(PinnedPlaybackChrome));
      final deckRect = tester.getRect(find.byType(MainWindowDeck));
      final stripRect = tester.getRect(find.byKey(analysisOverlayStripKey));
      final sidebarRect = tester.getRect(find.byKey(mainWindowListSidebarKey));

      expect(stripRect.top, greaterThanOrEqualTo(viewportRect.bottom));
      expect(chromeRect.top, greaterThanOrEqualTo(viewportRect.bottom));
      expect(deckRect.top, greaterThanOrEqualTo(chromeRect.bottom));
      expect(sidebarRect.right, lessThanOrEqualTo(stripRect.left));
      expect(sidebarRect.top, lessThanOrEqualTo(viewportRect.top));
      expect(sidebarRect.bottom, greaterThanOrEqualTo(stripRect.bottom));
      expect(find.byKey(handles.analysisOverlayButtonKey), findsOneWidget);
      expect(find.byKey(handles.controlsBarKey), findsOneWidget);
      expect(find.byKey(handles.timelineSliderKey), findsOneWidget);
      expect(find.byKey(handles.loopRangeBarKey), findsOneWidget);
      expect(find.text('Timeline'), findsNothing);
      expect(find.text('Analysis'), findsNothing);
    },
  );

  testWidgets(
    'analysis deck replaces the compact timeline and preserves playback',
    (tester) async {
      final feedback = AppFeedbackController();
      addTearDown(feedback.dispose);
      const mediaTrack = TrackEntry(
        TrackInfo(
          fileId: 1,
          slot: 0,
          path: 'track.mp4',
          width: 1920,
          height: 1080,
        ),
      );
      final handles = _handles();

      Widget build(MainWindowDeckTab tab) => _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(
              settingsVisible: false,
              tracks: const [mediaTrack],
              deckTab: tab,
            ),
            handles: handles,
            actions: _noop,
          ),
        ),
      );

      await tester.pumpWidget(build(MainWindowDeckTab.timeline));
      final timelineViewport = tester.getRect(find.byType(ViewportPanel));
      final timelineChrome = tester.getRect(find.byType(PinnedPlaybackChrome));
      final timelineDeck = tester.getRect(find.byType(MainWindowDeck));
      expect(find.byType(AnalysisWorkspacePage), findsNothing);

      await tester.pumpWidget(build(MainWindowDeckTab.analysis));
      await tester.pump();
      final analysisViewport = tester.getRect(find.byType(ViewportPanel));
      final analysisChrome = tester.getRect(find.byType(PinnedPlaybackChrome));
      final analysisDeck = tester.getRect(find.byType(MainWindowDeck));
      final chartShelf = tester.getRect(
        find.byKey(mainWindowAnalysisChartShelfKey),
      );
      final playbackAreaHeight =
          timelineViewport.height + timelineChrome.height + timelineDeck.height;

      expect(timelineDeck.height, timelineTrackRowHeight * 2);
      expect(analysisDeck.height, closeTo(playbackAreaHeight * 0.42, 0.1));
      expect(analysisViewport.height, lessThan(timelineViewport.height));
      expect(analysisViewport.width, timelineViewport.width);
      expect(analysisChrome.height, timelineChrome.height);
      expect(analysisChrome.width, timelineChrome.width);
      expect(chartShelf.width, analysisDeck.width);
      expect(find.byType(AnalysisWorkspacePage), findsOneWidget);
      expect(find.byKey(mainWindowAnalysisChartShelfKey), findsOneWidget);
      expect(find.byKey(mainWindowAnalysisNaluSidebarKey), findsNothing);
      expect(find.byType(LoopRangeBar), findsNothing);
      expect(find.byType(TimelineArea), findsNothing);
    },
  );

  testWidgets(
    'analysis workspace exposes tool tabs and closes back to timeline',
    (tester) async {
      final feedback = AppFeedbackController();
      addTearDown(feedback.dispose);
      final selected = <MainWindowDeckTab>[];
      const mediaTrack = TrackEntry(
        TrackInfo(
          fileId: 1,
          slot: 0,
          path: 'track.mp4',
          width: 1920,
          height: 1080,
        ),
      );

      await tester.pumpWidget(
        _localized(
          AppFeedbackScope(
            controller: feedback,
            child: MainWindowScaffold(
              model: _model(
                settingsVisible: false,
                tracks: const [mediaTrack],
                deckTab: MainWindowDeckTab.analysis,
              ),
              handles: _handles(),
              actions: _actionsWithDeck(onTabChanged: selected.add),
            ),
          ),
        ),
      );

      final analysisTabs = tester.widget<SegmentedButton<int>>(
        find
            .ancestor(
              of: find.byKey(const ValueKey('main-window-deck-tab-analysis')),
              matching: find.byType(SegmentedButton<int>),
            )
            .first,
      );
      expect(analysisTabs.selected, {0});
      expect(analysisTabs.onSelectionChanged, isNotNull);

      await tester.tap(
        find.byKey(const ValueKey('main-window-deck-tab-analysis')),
      );
      await tester.tap(
        find.byKey(const ValueKey('main-window-deck-tab-quality')),
      );
      await tester.tap(find.byKey(mainWindowDeckCollapseButtonKey));
      expect(selected, [MainWindowDeckTab.quality, MainWindowDeckTab.timeline]);
      expect(
        find.byKey(const ValueKey('main-window-deck-tab-timeline')),
        findsNothing,
      );
    },
  );

  testWidgets('analysis sidebar switches the focused track', (tester) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);
    const tracks = [
      TrackEntry(
        TrackInfo(
          fileId: 1,
          slot: 0,
          path: 'first.mp4',
          width: 1920,
          height: 1080,
        ),
      ),
      TrackEntry(
        TrackInfo(
          fileId: 2,
          slot: 1,
          path: 'second.mp4',
          width: 1280,
          height: 720,
        ),
      ),
    ];
    const entries = [
      AnalysisWorkspaceEntry(
        fileId: 1,
        path: 'first.mp4',
        fileName: 'first.mp4',
        hash: null,
        generationStatus: null,
      ),
      AnalysisWorkspaceEntry(
        fileId: 2,
        path: 'second.mp4',
        fileName: 'second.mp4',
        hash: null,
        generationStatus: null,
      ),
    ];

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(
              settingsVisible: false,
              tracks: tracks,
              analysisEntries: entries,
              deckTab: MainWindowDeckTab.analysis,
              marksSidebarVisible: true,
            ),
            handles: _handles(),
            actions: _noop,
          ),
        ),
      ),
    );
    await tester.pump();

    final selector = find.byKey(mainWindowAnalysisTrackSelectorKey);
    expect(find.byKey(mainWindowListNaluTabKey), findsOneWidget);
    expect(find.byKey(mainWindowAnalysisNaluSidebarKey), findsOneWidget);
    expect(selector, findsOneWidget);
    expect(find.text('1. first.mp4'), findsWidgets);

    await tester.tap(selector);
    await tester.pumpAndSettle();
    await tester.tap(find.text('2. second.mp4').last);
    await tester.pumpAndSettle();

    expect(tester.widget<DropdownMenu<int>>(selector).initialSelection, 1);
    expect(find.text('2. second.mp4'), findsWidgets);
  });

  testWidgets('analysis split keeps its pair and changes focused entry', (
    tester,
  ) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);
    const tracks = [
      TrackEntry(
        TrackInfo(
          fileId: 1,
          slot: 0,
          path: 'first.mp4',
          width: 1920,
          height: 1080,
        ),
      ),
      TrackEntry(
        TrackInfo(
          fileId: 2,
          slot: 1,
          path: 'second.mp4',
          width: 1280,
          height: 720,
        ),
      ),
      TrackEntry(
        TrackInfo(
          fileId: 3,
          slot: 2,
          path: 'third.mp4',
          width: 640,
          height: 360,
        ),
      ),
    ];
    const entries = [
      AnalysisWorkspaceEntry(
        fileId: 1,
        path: 'first.mp4',
        fileName: 'first.mp4',
        hash: null,
        generationStatus: null,
      ),
      AnalysisWorkspaceEntry(
        fileId: 2,
        path: 'second.mp4',
        fileName: 'second.mp4',
        hash: null,
        generationStatus: null,
      ),
      AnalysisWorkspaceEntry(
        fileId: 3,
        path: 'third.mp4',
        fileName: 'third.mp4',
        hash: null,
        generationStatus: null,
      ),
    ];

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(
              settingsVisible: false,
              tracks: tracks,
              analysisEntries: entries,
              deckTab: MainWindowDeckTab.analysis,
              marksSidebarVisible: true,
            ),
            handles: _handles(),
            actions: _noop,
          ),
        ),
      ),
    );
    await tester.pump();

    final modeToggle = find.byKey(mainWindowAnalysisViewModeToggleKey);
    expect(modeToggle, findsOneWidget);
    await tester.tap(
      find.descendant(of: modeToggle, matching: find.text('Split')),
    );
    await tester.pump();

    final firstCell = find.byKey(analysisWorkspaceSplitCellKey(1));
    final secondCell = find.byKey(analysisWorkspaceSplitCellKey(2));
    expect(firstCell, findsOneWidget);
    expect(secondCell, findsOneWidget);
    expect(find.byKey(analysisWorkspaceSplitCellKey(3)), findsNothing);
    expect(find.byKey(analysisWorkspaceSplitDividerKey), findsOneWidget);
    expect(
      find.descendant(
        of: find.byKey(analysisWorkspaceFocusedSplitCellKey),
        matching: firstCell,
      ),
      findsOneWidget,
    );

    await tester.tap(secondCell);
    await tester.pump();
    await tester.pump();

    expect(firstCell, findsOneWidget);
    expect(secondCell, findsOneWidget);
    expect(
      find.descendant(
        of: find.byKey(analysisWorkspaceFocusedSplitCellKey),
        matching: secondCell,
      ),
      findsOneWidget,
    );
    expect(
      tester
          .widget<DropdownMenu<int>>(
            find.byKey(mainWindowAnalysisTrackSelectorKey),
          )
          .initialSelection,
      1,
    );
  });

  testWidgets('left sidebar follows analysis context and restores its tab', (
    tester,
  ) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);
    const track = TrackEntry(
      TrackInfo(
        fileId: 1,
        slot: 0,
        path: 'track.mp4',
        width: 1920,
        height: 1080,
      ),
    );
    const entry = AnalysisWorkspaceEntry(
      fileId: 1,
      path: 'track.mp4',
      fileName: 'track.mp4',
      hash: null,
      generationStatus: null,
    );

    Widget build(MainWindowDeckTab tab) => _localized(
      AppFeedbackScope(
        controller: feedback,
        child: MainWindowScaffold(
          model: _model(
            settingsVisible: false,
            tracks: const [track],
            analysisEntries: const [entry],
            deckTab: tab,
            marksSidebarVisible: true,
          ),
          handles: _handles(),
          actions: _noop,
        ),
      ),
    );

    await tester.pumpWidget(build(MainWindowDeckTab.timeline));
    await tester.tap(find.byKey(mainWindowListTracksTabKey));
    await tester.pump();
    expect(
      find.byKey(const ValueKey('main-window-track-row-1')),
      findsOneWidget,
    );

    await tester.pumpWidget(build(MainWindowDeckTab.analysis));
    await tester.pump();
    expect(find.byKey(mainWindowAnalysisNaluSidebarKey), findsOneWidget);
    expect(find.byKey(mainWindowAnalysisTrackSelectorKey), findsOneWidget);
    expect(find.byKey(const ValueKey('main-window-track-row-1')), findsNothing);

    await tester.pumpWidget(build(MainWindowDeckTab.timeline));
    await tester.pump();
    expect(find.byKey(mainWindowAnalysisNaluSidebarKey), findsNothing);
    expect(
      find.byKey(const ValueKey('main-window-track-row-1')),
      findsOneWidget,
    );
  });

  testWidgets(
    'quality deck analyzes proxies, seeks samples, and requests metric marks',
    (tester) async {
      final feedback = AppFeedbackController();
      addTearDown(feedback.dispose);
      final seeks = <(int, int)>[];
      final markRequests = <MainWindowQualityMarkRequest>[];
      const mediaTrack = TrackEntry(
        TrackInfo(
          fileId: 7,
          slot: 0,
          path: 'quality.mp4',
          width: 1920,
          height: 1080,
        ),
      );

      await tester.pumpWidget(
        _localized(
          AppFeedbackScope(
            controller: feedback,
            child: MainWindowScaffold(
              model: _model(
                settingsVisible: false,
                tracks: const [mediaTrack],
                deckTab: MainWindowDeckTab.quality,
                qualityDataSource: const _FakeQualityDataSource(),
              ),
              handles: _handles(),
              actions: _actionsWithDeck(
                onQualitySeekRequested: (fileId, ptsUs) {
                  seeks.add((fileId, ptsUs));
                },
                onQualityMarksRequested: (request) async {
                  markRequests.add(request);
                  return 1;
                },
              ),
            ),
          ),
        ),
      );

      expect(find.text('Quality'), findsOneWidget);
      expect(find.textContaining('Experimental proxies'), findsOneWidget);
      expect(find.byKey(mainWindowAnalysisNaluSidebarKey), findsNothing);
      expect(find.byType(DropdownMenu<int>), findsOneWidget);

      await tester.tap(find.byKey(mainWindowQualityAnalyzeButtonKey));
      await tester.pump();

      expect(find.byKey(mainWindowQualityChartKey), findsOneWidget);
      expect(find.textContaining('3 samples'), findsOneWidget);
      expect(find.text('Create 1 mark'), findsOneWidget);

      final blockingLegend = find.byKey(
        const ValueKey('main-window-quality-metric-blockiness'),
      );
      expect(
        tester.widget<Semantics>(blockingLegend).properties.toggled,
        isTrue,
      );
      final blockingRect = tester.getRect(blockingLegend);
      await tester.tapAt(
        Offset(blockingRect.left + 10, blockingRect.center.dy),
      );
      await tester.pump();
      expect(
        tester.widget<Semantics>(blockingLegend).properties.toggled,
        isFalse,
      );
      await tester.tapAt(
        Offset(blockingRect.left + 40, blockingRect.center.dy),
      );
      await tester.pump();
      expect(
        tester.widget<Semantics>(blockingLegend).properties.toggled,
        isTrue,
      );

      await tester.drag(
        find.byKey(mainWindowQualityThresholdDragKey),
        const Offset(0, 24),
      );
      await tester.pump();

      final chartRect = tester.getRect(find.byKey(mainWindowQualityChartKey));
      await tester.tapAt(Offset(chartRect.right - 14, chartRect.center.dy));
      await tester.pump();
      expect(seeks, [(7, 2000000)]);

      await tester.tap(find.byKey(mainWindowQualityCreateMarksButtonKey));
      await tester.pump();
      expect(markRequests, hasLength(1));
      expect(markRequests.single.fileId, 7);
      expect(markRequests.single.metric, AnalysisQualityMetric.blockiness);
      expect(markRequests.single.threshold, lessThan(0.3));
    },
  );

  testWidgets('analysis shelf and close expose bounded local actions', (
    tester,
  ) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);
    const mediaTrack = TrackEntry(
      TrackInfo(
        fileId: 1,
        slot: 0,
        path: 'track.mp4',
        width: 1920,
        height: 1080,
      ),
    );
    final tabs = <MainWindowDeckTab>[];
    final heights = <double>[];

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(
              settingsVisible: false,
              tracks: const [mediaTrack],
              deckTab: MainWindowDeckTab.analysis,
            ),
            handles: _handles(),
            actions: _actionsWithDeck(
              onTabChanged: tabs.add,
              onHeightChanged: heights.add,
            ),
          ),
        ),
      ),
    );

    expect(find.byKey(mainWindowAnalysisChartShelfKey), findsOneWidget);
    expect(find.byKey(mainWindowDeckResizeHandleKey), findsOneWidget);
    final initialHeight = tester.getSize(find.byType(MainWindowDeck)).height;
    final availableHeight =
        tester.getSize(find.byType(ViewportPanel)).height +
        tester.getSize(find.byType(PinnedPlaybackChrome)).height +
        initialHeight;
    expect(initialHeight, closeTo(availableHeight * 0.42, 0.1));

    await tester.drag(
      find.byKey(mainWindowDeckResizeHandleKey),
      const Offset(0, -80),
    );
    await tester.pump();
    expect(
      tester.getSize(find.byType(MainWindowDeck)).height,
      closeTo(initialHeight + 80, 0.1),
    );
    expect(heights, isNotEmpty);

    await tester.drag(
      find.byKey(mainWindowDeckResizeHandleKey),
      const Offset(0, 1000),
    );
    await tester.pump();
    expect(
      tester.getSize(find.byType(MainWindowDeck)).height,
      closeTo(availableHeight * 0.25, 0.1),
    );

    await tester.tap(find.byKey(mainWindowDeckCollapseButtonKey));
    expect(tabs, [MainWindowDeckTab.timeline]);
  });

  testWidgets('timeline height follows track count and caps visible rows', (
    tester,
  ) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);

    TrackEntry track(int fileId) => TrackEntry(
      TrackInfo(
        fileId: fileId,
        slot: fileId - 1,
        path: 'track-$fileId.mp4',
        width: 1920,
        height: 1080,
      ),
    );

    Widget build(List<TrackEntry> tracks) => _localized(
      AppFeedbackScope(
        controller: feedback,
        child: MainWindowScaffold(
          model: _model(settingsVisible: false, tracks: tracks),
          handles: _handles(),
          actions: _noop,
        ),
      ),
    );

    await tester.pumpWidget(build([track(1)]));
    expect(
      tester.getSize(find.byType(MainWindowDeck)).height,
      timelineTrackRowHeight * 2,
    );

    await tester.pumpWidget(
      build([for (var fileId = 1; fileId <= 6; fileId++) track(fileId)]),
    );
    expect(
      tester.getSize(find.byType(MainWindowDeck)).height,
      timelineTrackRowHeight * 5,
    );
  });

  testWidgets('marks sidebar renders with list header actions', (tester) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(settingsVisible: false, marksSidebarVisible: true),
            handles: _handles(),
            actions: _noop,
          ),
        ),
      ),
    );

    expect(find.text('All marks'), findsOneWidget);
    expect(find.text('0 total'), findsOneWidget);
    expect(find.byTooltip('Select all marks'), findsOneWidget);
    expect(find.byTooltip('Cancel mark selection'), findsOneWidget);
    expect(find.byTooltip('Delete selected marks'), findsOneWidget);
    final sidebarRect = tester.getRect(find.byKey(mainWindowListSidebarKey));
    final viewportRect = tester.getRect(find.byType(ViewportPanel));
    expect(sidebarRect.right, lessThanOrEqualTo(viewportRect.left));
    expect(find.byKey(mainWindowInspectorKey), findsNothing);
  });

  testWidgets('track list selects a track from the left sidebar', (
    tester,
  ) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);
    final selected = <int?>[];
    const track = TrackEntry(
      TrackInfo(
        fileId: 17,
        slot: 0,
        path: r'C:\media\sample.mp4',
        width: 1920,
        height: 1080,
        codecName: 'h264',
      ),
    );

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(
              settingsVisible: false,
              marksSidebarVisible: true,
              tracks: const [track],
            ),
            handles: _handles(),
            actions: _noopWithListActions(selected.add),
          ),
        ),
      ),
    );

    await tester.tap(find.byKey(mainWindowListTracksTabKey));
    await tester.pump();
    final row = find.byKey(const ValueKey('main-window-track-row-17'));
    expect(
      find.descendant(of: row, matching: find.text('sample.mp4')),
      findsOneWidget,
    );
    expect(
      find.descendant(of: row, matching: find.text('1920 × 1080 · H264')),
      findsOneWidget,
    );

    await tester.tap(row);
    expect(selected, [17]);
  });

  testWidgets('track selection opens metadata in the right inspector', (
    tester,
  ) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);
    const track = TrackInfo(
      fileId: 17,
      slot: 0,
      path: r'C:\media\sample.mp4',
      width: 1920,
      height: 1080,
      durationUs: 62500000,
      bitRate: 4800000,
      formatName: 'mov,mp4',
      codecName: 'h264',
      decoderName: 'd3d11va',
    );

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(
              settingsVisible: false,
              tracks: const [TrackEntry(track)],
              selection: const MainWindowTrackSelection(track),
            ),
            handles: _handles(),
            actions: _noop,
          ),
        ),
      ),
    );

    expect(find.byKey(mainWindowInspectorKey), findsOneWidget);
    expect(find.text('Track properties'), findsOneWidget);
    expect(find.text('sample.mp4'), findsWidgets);
    expect(find.text('mov,mp4'), findsOneWidget);
    expect(find.text('4.80 Mbps'), findsOneWidget);
    expect(find.byKey(mainWindowListSidebarKey), findsNothing);
  });

  testWidgets('analysis selection opens the shared right inspector', (
    tester,
  ) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);
    var closed = false;
    final selection = AnalysisNaluSelection(
      fileId: 1,
      codec: AnalysisCodec.h264,
      naluIndex: 2,
      nalu: NaluInfo(
        offset: 24,
        size: 16,
        nalType: 7,
        temporalId: 0,
        layerId: 0,
        flags: 0,
      ),
    );

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(
              settingsVisible: false,
              selection: MainWindowAnalysisSelection(selection),
            ),
            handles: _handles(),
            actions: _noopWithOverlayActions(
              onCloseInspector: () => closed = true,
            ),
          ),
        ),
      ),
    );

    expect(find.byKey(mainWindowInspectorKey), findsOneWidget);
    expect(find.byKey(analysisNaluDetailPanelKey), findsOneWidget);
    expect(find.byType(QuickMarkSidebar), findsNothing);

    await tester.tap(find.byKey(mainWindowInspectorCloseKey));
    expect(closed, isTrue);
  });

  testWidgets(
    'macOS native compositor hides Flutter texture only while active',
    (tester) async {
      if (!Platform.isMacOS) return;
      final feedback = AppFeedbackController();
      addTearDown(feedback.dispose);

      await tester.pumpWidget(
        _localized(
          AppFeedbackScope(
            controller: feedback,
            child: MainWindowScaffold(
              model: _model(
                settingsVisible: false,
                textureId: 7,
                viewportState: const ViewportDisplayState.active(),
                nativeCompositorActive: false,
              ),
              handles: _handles(),
              actions: _noop,
            ),
          ),
        ),
      );

      expect(find.byType(Texture), findsOneWidget);

      await tester.pumpWidget(
        _localized(
          AppFeedbackScope(
            controller: feedback,
            child: MainWindowScaffold(
              model: _model(
                settingsVisible: false,
                textureId: 7,
                viewportState: const ViewportDisplayState.active(),
                nativeCompositorActive: true,
              ),
              handles: _handles(),
              actions: _noop,
            ),
          ),
        ),
      );

      expect(find.byType(Texture), findsNothing);
    },
  );

  testWidgets(
    'native compositor viewport rect follows sidebar and timeline layout',
    (tester) async {
      if (!Platform.isMacOS) return;
      final feedback = AppFeedbackController();
      addTearDown(feedback.dispose);
      final rects = <({int left, int top, int width, int height})>[];
      final actions = _actionsWithViewportRect((
        left,
        top,
        width,
        height,
        surfaceWidth,
        surfaceHeight,
      ) {
        rects.add((left: left, top: top, width: width, height: height));
      });
      const mediaTrack = TrackEntry(
        TrackInfo(
          fileId: 1,
          slot: 0,
          path: 'track.mp4',
          width: 1920,
          height: 1080,
        ),
      );

      Widget build({required bool sidebar}) => _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(
              settingsVisible: false,
              textureId: 7,
              viewportState: const ViewportDisplayState.active(),
              nativeCompositorActive: true,
              marksSidebarVisible: sidebar,
              tracks: [mediaTrack],
            ),
            handles: _handles(),
            actions: actions,
          ),
        ),
      );

      await tester.pumpWidget(build(sidebar: false));
      await tester.pump();
      expect(rects, isNotEmpty);
      final withoutSidebar = rects.last;

      await tester.pumpWidget(build(sidebar: true));
      await tester.pump();
      expect(rects.last.width, lessThan(withoutSidebar.width));

      final viewportRect = tester.getRect(find.byType(ViewportPanel));
      final sidebarRect = tester.getRect(find.byType(QuickMarkSidebar));
      final timelineRect = tester.getRect(find.byType(MainWindowDeck));
      final devicePixelRatio = tester.view.devicePixelRatio;
      final reported = rects.last;

      expect(reported.left, (viewportRect.left * devicePixelRatio).round());
      expect(reported.top, (viewportRect.top * devicePixelRatio).round());
      expect(reported.width, (viewportRect.width * devicePixelRatio).round());
      expect(reported.height, (viewportRect.height * devicePixelRatio).round());
      expect(sidebarRect.right, lessThanOrEqualTo(viewportRect.left));
      expect(viewportRect.bottom, lessThanOrEqualTo(timelineRect.top));
    },
  );

  testWidgets('marks sidebar row tap jumps to mark', (tester) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);
    final selectedVisible = <int?>[];
    final jumped = <int>[];

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(
              settingsVisible: false,
              marksSidebarVisible: true,
              quickMarks: [
                _quickMark(1, const Rect.fromLTWH(0.1, 0.2, 0.2, 0.3)),
              ],
              tracksByFileId: const {
                1: TrackInfo(
                  fileId: 1,
                  slot: 0,
                  path: '/tmp/clip.mp4',
                  width: 1920,
                  height: 1080,
                ),
              },
            ),
            handles: _handles(),
            actions: _noopWithMarkActions(
              onJumpToMark: jumped.add,
              onSelectVisibleMark: selectedVisible.add,
            ),
          ),
        ),
      ),
    );

    expect(find.textContaining('Track 1'), findsWidgets);
    expect(
      find.descendant(
        of: find.byKey(const ValueKey('quick-mark-sidebar-row-1')),
        matching: find.byIcon(Icons.circle),
      ),
      findsNothing,
    );

    await tester.tap(find.byKey(const ValueKey('quick-mark-sidebar-row-1')));
    await tester.pump();

    expect(selectedVisible, isEmpty);
    expect(jumped, equals([1]));
    expect(
      find.byKey(const ValueKey('quick-mark-sidebar-jump-1')),
      findsNothing,
    );
  });

  testWidgets('marks sidebar scrolls selected mark into view', (tester) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);
    final marks = [
      for (var i = 1; i <= 24; i++)
        _quickMark(
          i,
          Rect.fromLTWH(0.1, 0.02 * i, 0.2, 0.03),
          ptsUs: i * 1000000,
        ),
    ];

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(
              settingsVisible: false,
              marksSidebarVisible: true,
              quickMarks: marks,
              selectedQuickMarkId: 24,
              tracksByFileId: const {
                1: TrackInfo(
                  fileId: 1,
                  slot: 0,
                  path: '/tmp/clip.mp4',
                  width: 1920,
                  height: 1080,
                ),
              },
            ),
            handles: _handles(),
            actions: _noopWithMarkActions(),
          ),
        ),
      ),
    );

    await tester.pump();
    await tester.pump(const Duration(milliseconds: 220));

    final selectedRow = find.byKey(const ValueKey('quick-mark-sidebar-row-24'));
    expect(selectedRow, findsOneWidget);
    final sidebar = tester.getRect(find.byType(QuickMarkSidebar));
    final row = tester.getRect(selectedRow);
    expect(row.top, greaterThanOrEqualTo(sidebar.top));
    expect(row.bottom, lessThanOrEqualTo(sidebar.bottom));
  });

  testWidgets(
    'marks sidebar selection clears focus and deletes selected marks',
    (tester) async {
      final feedback = AppFeedbackController();
      addTearDown(feedback.dispose);
      final selectedVisible = <int?>[];
      final deleted = <int>[];
      final marks = [
        _quickMark(1, const Rect.fromLTWH(0.1, 0.2, 0.2, 0.3)),
        _quickMark(2, const Rect.fromLTWH(0.4, 0.2, 0.1, 0.2)),
      ];

      await tester.pumpWidget(
        _localized(
          AppFeedbackScope(
            controller: feedback,
            child: MainWindowScaffold(
              model: _model(
                settingsVisible: false,
                marksSidebarVisible: true,
                quickMarks: marks,
                selectedQuickMarkId: 1,
              ),
              handles: _handles(),
              actions: _noopWithMarkActions(
                onSelectVisibleMark: selectedVisible.add,
                onMarkDeleted: deleted.add,
              ),
            ),
          ),
        ),
      );

      await tester.tap(
        find.byKey(const ValueKey('quick-mark-sidebar-checkbox-1')),
      );
      await tester.pump();

      expect(selectedVisible, equals([null]));
      expect(find.text('1 selected'), findsOneWidget);

      await tester.tap(find.byTooltip('Delete selected marks'));
      expect(deleted, equals([1]));
    },
  );

  testWidgets('marks sidebar annotation enter respects IME composing', (
    tester,
  ) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(
              settingsVisible: false,
              marksSidebarVisible: true,
              quickMarks: [
                _quickMark(1, const Rect.fromLTWH(0.1, 0.2, 0.2, 0.3)),
              ],
              selectedQuickMarkId: 1,
            ),
            handles: _handles(),
            actions: _noop,
          ),
        ),
      ),
    );

    await tester.tap(find.byType(TextField).last);
    await tester.pump();

    var editor = tester.widget<EditableText>(find.byType(EditableText).last);
    expect(editor.focusNode.hasFocus, isTrue);

    editor.controller.value = const TextEditingValue(
      text: 'nihao',
      selection: TextSelection.collapsed(offset: 5),
      composing: TextRange(start: 0, end: 5),
    );
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();

    editor = tester.widget<EditableText>(find.byType(EditableText).last);
    expect(editor.focusNode.hasFocus, isTrue);

    editor.controller.value = const TextEditingValue(
      text: 'nihao',
      selection: TextSelection.collapsed(offset: 5),
    );
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();

    editor = tester.widget<EditableText>(find.byType(EditableText).last);
    expect(editor.focusNode.hasFocus, isFalse);
  });

  testWidgets('marks sidebar splitter changes sidebar width', (tester) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);
    final widths = <double>[];

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(settingsVisible: false, marksSidebarVisible: true),
            handles: _handles(),
            actions: _noopWithOverlayActions(
              onMarksSidebarWidthChanged: widths.add,
            ),
          ),
        ),
      ),
    );

    final sidebar = find.byType(QuickMarkSidebar);
    final splitterX = tester.getTopRight(sidebar).dx + 4;
    final splitterY = tester.getCenter(sidebar).dy;
    final gesture = await tester.startGesture(Offset(splitterX, splitterY));
    await gesture.moveBy(const Offset(24, 0));
    await gesture.up();
    await tester.pump();

    expect(widths, isNotEmpty);
    expect(widths.last, greaterThan(kDefaultMarksSidebarWidth));
  });
}

Widget _localized(Widget child) => MaterialApp(
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  home: child,
);

MainWindowViewHandles _handles() => MainWindowViewHandles(
  fullFrameCaptureKey: GlobalKey(),
  viewportKey: GlobalKey(),
  analysisOverlayButtonKey: GlobalKey(),
  timelineSliderKey: GlobalKey(),
  controlsBarKey: GlobalKey(),
  loopRangeBarKey: GlobalKey(),
  timelineHoverListenable: ValueNotifier(const TimelineHoverState()),
);

MainWindowViewModel _model({
  required bool settingsVisible,
  int? textureId,
  ViewportDisplayState viewportState = const ViewportDisplayState.empty(),
  bool nativeCompositorActive = false,
  bool marksSidebarVisible = false,
  bool analysisOverlayControlsVisible = false,
  List<TrackEntry> tracks = const [],
  List<QuickMark> quickMarks = const [],
  int? selectedQuickMarkId,
  Map<int, TrackInfo> tracksByFileId = const {},
  MainWindowDeckTab deckTab = MainWindowDeckTab.timeline,
  double deckHeight = kDefaultDeckHeight,
  bool deckCollapsed = false,
  MainWindowSelection? selection,
  AnalysisQualityDataSource? qualityDataSource,
  List<AnalysisWorkspaceEntry> analysisEntries = const [],
}) => MainWindowViewModel(
  session: MainWindowSessionVm.fromSession(const PlaybackSession.normal()),
  viewport: MainWindowViewportVm(
    viewMode: 0,
    viewModeEnabled: true,
    textureId: textureId,
    nativeCompositorActive: nativeCompositorActive,
    viewportState: viewportState,
    layout: const LayoutState(),
    tracks: tracks
        .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry.info))
        .toList(),
    quickMarks: const [],
    quickMarkDraft: null,
    selectedQuickMarkId: null,
  ),
  marks: MainWindowMarksVm(
    allMarks: quickMarks,
    visibleMarks: quickMarks,
    visibleMarkIds: quickMarks.map((mark) => mark.id).toSet(),
    selectedMarkId: selectedQuickMarkId,
    tracksByFileId: tracksByFileId,
    thumbnailsByMarkId: const {},
    currentPtsUs: 0,
  ),
  media: MainWindowMediaVm(
    analysisEnabled: true,
    analysisOverlayEnabled: true,
    nativePlaybackAvailable: true,
    localFilePlaybackAvailable: true,
    networkMediaAvailable: true,
    sshRemoteMediaAvailable: true,
    nativeFilePickerAvailable: true,
    localFilePlaybackCapability:
        PlatformCapabilities.windows.localFilePlaybackCapability,
    networkMediaPlaybackCapability:
        PlatformCapabilities.windows.networkMediaPlaybackCapability,
    sshRemoteMediaPlaybackCapability:
        PlatformCapabilities.windows.sshRemoteMediaPlaybackCapability,
    nativeFilePickerCapability:
        PlatformCapabilities.windows.nativeFilePickerCapability,
    analysisOverlaysCapability:
        PlatformCapabilities.windows.analysisOverlaysCapability,
    tracks: tracks,
    syncOffsets: const {},
    audibleTrackFileId: null,
    performanceAlertPolicy: PerformanceAlertPolicy.sustained,
    analysisDataSource: _FakeAnalysisToolbarDataSource(),
  ),
  playback: MainWindowPlaybackVm(
    timelineStartWidth: 280,
    isPlaying: false,
    currentPtsUs: 0,
    durationUs: 0,
    markerUs: const [],
    seekMinUs: null,
    seekMaxUs: null,
    loopRangeEnabled: false,
    loopStartUs: 0,
    loopEndUs: 0,
    controlsWidth: 280,
  ),
  deck: MainWindowDeckVm(
    tab: deckTab,
    height: deckHeight,
    collapsed: deckCollapsed,
    analysisEntries: ValueNotifier(analysisEntries),
    analysisTestHosts: AnalysisTestHostRegistry(),
    qualityDataSource:
        qualityDataSource ?? const NativeAnalysisQualityService(),
  ),
  selection:
      selection ?? _selectionForQuickMark(selectedQuickMarkId, quickMarks),
  overlays: MainWindowOverlayVm(
    dragging: false,
    mediaInfoVisible: false,
    profilerVisible: false,
    settingsVisible: settingsVisible,
    analysisOverlayControlsVisible: analysisOverlayControlsVisible,
    marksSidebarVisible: marksSidebarVisible,
    marksSidebarWidth: kDefaultMarksSidebarWidth,
    fullScreen: false,
    fullScreenControlsVisible: false,
  ),
);

MainWindowSelection _selectionForQuickMark(
  int? selectedQuickMarkId,
  List<QuickMark> quickMarks,
) {
  if (selectedQuickMarkId == null) return const MainWindowNoSelection();
  for (final mark in quickMarks) {
    if (mark.id == selectedQuickMarkId) {
      return MainWindowQuickMarkSelection(mark);
    }
  }
  return const MainWindowNoSelection();
}

QuickMark _quickMark(int id, Rect rect, {int ptsUs = 0}) {
  return QuickMark(
    id: id,
    anchor: QuickMarkAnchor(fileId: 1, ptsUs: ptsUs, dtsUs: ptsUs),
    sourceRect: rect,
  );
}

final _noop = MainWindowViewActions(
  drop: MainWindowDropActions(
    filesDropped: (_) {},
    dragEntered: () {},
    dragExited: () {},
  ),
  toolbar: MainWindowToolbarActions(
    onViewModeChanged: (_) {},
    onOpenFile: () async {},
    onOpenNetworkMedia: (_) async {},
    onOpenSshRemoteMedia: (_) async {},
    onMediaInfo: () {},
    onAnalysis: () async {},
    onAnalysisOverlayPanelToggle: () async {},
    onProfiler: () {},
    onSettings: () {},
    onMarksSidebarToggle: () {},
  ),
  viewport: MainWindowViewportActions(
    onPan: (_) {},
    onSplit: (_) {},
    onZoom: (_, _) {},
    onPointerButton: (_, _) {},
    onResize: (_, _, _) {},
    onNativeCompositorViewportRect: (_, _, _, _, _, _) {},
    onQuickMarkStart: (_) {},
    onQuickMarkUpdate: (_) {},
    onQuickMarkInteraction: () {},
    onQuickMarkEnd: () {},
    onQuickMarkCancel: () {},
    onQuickMarkSelect: (_) {},
    onQuickMarkChanged: (_) {},
    onQuickMarkDeleted: (_) {},
    onQuickMarkFocus: (_) {},
  ),
  marks: MainWindowMarksActions(
    onJumpToMark: (_) {},
    onSelectVisibleMark: (_) {},
    onMarkChanged: (_) {},
    onMarkDeleted: (_) {},
    onFocusVisibleMark: (_) {},
  ),
  mediaTimeline: MainWindowMediaTimelineActions(
    onMediaSwapped: (_, _) {},
    onRemoveTrack: (_) async {},
    onZoomChanged: (_) {},
    onToggleFullScreen: () {},
    onTogglePlay: () async {},
    onStepForward: () async {},
    onStepBackward: () async {},
    onSeek: (_) {},
    onSliderHover: (_, _) {},
    onLoopRangeEnabledChanged: (_) async {},
    onLoopRangeChanged: (_, _) {},
    onLoopRangeChangeEnd: null,
    onReorder: (_, _) {},
    onOffsetChanged: (_, _) async {},
    onToggleTrackAudio: (_) {},
    onControlsWidthChanged: (_) {},
  ),
  deck: MainWindowDeckActions(
    onTabChanged: (_) {},
    onHeightChanged: (_) {},
    onCollapsedChanged: (_) {},
  ),
  analysisOverlay: MainWindowAnalysisOverlayActions(
    onTypeChanged: (_) {},
    onOpacityChanged: (_) {},
    onActivate: () async {},
    onClose: () {},
  ),
  overlays: MainWindowOverlayActions(
    onCloseMediaInfo: () {},
    onCloseProfiler: () {},
    onCloseSettings: () {},
    onCloseMarksSidebar: () {},
    onMarksSidebarWidthChanged: (_) {},
    onViewportPixelSizeModeChanged: (_) {},
    onPerformanceAlertPolicyChanged: (_) {},
    onFullScreenPointerActivity: () {},
    onFullScreenControlsHoverChanged: (_) {},
  ),
);

MainWindowViewActions _actionsWithDeck({
  ValueChanged<MainWindowDeckTab>? onTabChanged,
  ValueChanged<double>? onHeightChanged,
  ValueChanged<bool>? onCollapsedChanged,
  void Function(int fileId, int trackPtsUs)? onQualitySeekRequested,
  Future<int> Function(MainWindowQualityMarkRequest request)?
  onQualityMarksRequested,
}) {
  return MainWindowViewActions(
    drop: _noop.drop,
    toolbar: _noop.toolbar,
    viewport: _noop.viewport,
    marks: _noop.marks,
    mediaTimeline: _noop.mediaTimeline,
    deck: MainWindowDeckActions(
      onTabChanged: onTabChanged ?? _noop.deck.onTabChanged,
      onHeightChanged: onHeightChanged ?? _noop.deck.onHeightChanged,
      onCollapsedChanged: onCollapsedChanged ?? _noop.deck.onCollapsedChanged,
      onQualitySeekRequested: onQualitySeekRequested,
      onQualityMarksRequested: onQualityMarksRequested,
    ),
    analysisOverlay: _noop.analysisOverlay,
    overlays: _noop.overlays,
  );
}

class _FakeQualityDataSource
    implements AnalysisQualityDataSource, AnalysisQualityFrameDataSource {
  const _FakeQualityDataSource();

  @override
  Future<AnalysisQualityReport> analyze(AnalysisQualityRequest request) async {
    return const AnalysisQualityReport(
      schemaVersion: 4,
      videoWidth: 1920,
      videoHeight: 1080,
      bitDepth: 8,
      sampleIntervalUs: 1000000,
      maxSamples: 600,
      truncated: false,
      unsupportedPixelFrames: 0,
      distributions: {
        AnalysisQualityMetric.blockiness: AnalysisQualityDistribution(
          count: 3,
          mean: 0.2,
          p95: 0.3,
          maximum: 0.4,
        ),
        AnalysisQualityMetric.banding: AnalysisQualityDistribution(
          count: 3,
          mean: 0.1,
          p95: 0.2,
          maximum: 0.25,
        ),
        AnalysisQualityMetric.blur: AnalysisQualityDistribution(
          count: 3,
          mean: 0.1,
          p95: 0.2,
          maximum: 0.25,
        ),
        AnalysisQualityMetric.noise: AnalysisQualityDistribution(
          count: 3,
          mean: 0.1,
          p95: 0.2,
          maximum: 0.25,
        ),
        AnalysisQualityMetric.flicker: AnalysisQualityDistribution(
          count: 2,
          mean: 0.1,
          p95: 0.2,
          maximum: 0.25,
        ),
      },
      samples: [
        AnalysisQualitySample(
          sampleIndex: 0,
          decodedFrameIndex: 0,
          ptsUs: 0,
          blockiness: 0.1,
          banding: 0.1,
          blur: 0.1,
          noise: 0.1,
          flicker: null,
          averageQp: null,
        ),
        AnalysisQualitySample(
          sampleIndex: 1,
          decodedFrameIndex: 30,
          ptsUs: 1000000,
          blockiness: 0.2,
          banding: 0.1,
          blur: 0.1,
          noise: 0.1,
          flicker: 0.1,
          averageQp: null,
        ),
        AnalysisQualitySample(
          sampleIndex: 2,
          decodedFrameIndex: 60,
          ptsUs: 2000000,
          blockiness: 0.4,
          banding: 0.2,
          blur: 0.2,
          noise: 0.2,
          flicker: 0.2,
          averageQp: null,
        ),
      ],
    );
  }

  @override
  Future<Map<int, FrameInfo>> readCachedFrames(
    String videoPath,
    List<AnalysisQualitySample> samples,
  ) async {
    return {
      for (final sample in samples)
        sample.decodedFrameIndex: FrameInfo(
          poc: sample.decodedFrameIndex,
          temporalId: sample.sampleIndex % 2,
          sliceType: sample.sampleIndex == 0 ? 2 : 1,
          nalType: 1,
          avgQp: 22 + sample.sampleIndex,
          numRefL0: 0,
          numRefL1: 0,
          refPocsL0: const [],
          refPocsL1: const [],
          pts: sample.ptsUs,
          dts: sample.ptsUs,
          packetSize: 1000 + sample.sampleIndex * 250,
          keyframe: sample.sampleIndex == 0 ? 1 : 0,
        ),
    };
  }
}

MainWindowViewActions _actionsWithViewportRect(
  void Function(
    int left,
    int top,
    int width,
    int height,
    int surfaceWidth,
    int surfaceHeight,
  )
  onNativeCompositorViewportRect,
) {
  return MainWindowViewActions(
    drop: _noop.drop,
    toolbar: _noop.toolbar,
    viewport: MainWindowViewportActions(
      onPan: _noop.viewport.onPan,
      onSplit: _noop.viewport.onSplit,
      onZoom: _noop.viewport.onZoom,
      onPointerButton: _noop.viewport.onPointerButton,
      onResize: _noop.viewport.onResize,
      onNativeCompositorViewportRect: onNativeCompositorViewportRect,
      onQuickMarkStart: _noop.viewport.onQuickMarkStart,
      onQuickMarkUpdate: _noop.viewport.onQuickMarkUpdate,
      onQuickMarkInteraction: _noop.viewport.onQuickMarkInteraction,
      onQuickMarkEnd: _noop.viewport.onQuickMarkEnd,
      onQuickMarkCancel: _noop.viewport.onQuickMarkCancel,
      onQuickMarkSelect: _noop.viewport.onQuickMarkSelect,
      onQuickMarkChanged: _noop.viewport.onQuickMarkChanged,
      onQuickMarkDeleted: _noop.viewport.onQuickMarkDeleted,
      onQuickMarkFocus: _noop.viewport.onQuickMarkFocus,
    ),
    marks: _noop.marks,
    mediaTimeline: _noop.mediaTimeline,
    deck: _noop.deck,
    analysisOverlay: _noop.analysisOverlay,
    overlays: _noop.overlays,
  );
}

MainWindowViewActions _noopWithMarkActions({
  ValueChanged<int>? onJumpToMark,
  ValueChanged<int?>? onSelectVisibleMark,
  ValueChanged<int>? onMarkDeleted,
}) {
  return MainWindowViewActions(
    drop: _noop.drop,
    toolbar: _noop.toolbar,
    viewport: _noop.viewport,
    marks: MainWindowMarksActions(
      onJumpToMark: onJumpToMark ?? _noop.marks.onJumpToMark,
      onSelectVisibleMark:
          onSelectVisibleMark ?? _noop.marks.onSelectVisibleMark,
      onMarkChanged: _noop.marks.onMarkChanged,
      onMarkDeleted: onMarkDeleted ?? _noop.marks.onMarkDeleted,
      onFocusVisibleMark: _noop.marks.onFocusVisibleMark,
    ),
    mediaTimeline: _noop.mediaTimeline,
    deck: _noop.deck,
    analysisOverlay: _noop.analysisOverlay,
    overlays: _noop.overlays,
  );
}

MainWindowViewActions _noopWithListActions(ValueChanged<int?> onTrackSelected) {
  return MainWindowViewActions(
    drop: _noop.drop,
    toolbar: _noop.toolbar,
    viewport: _noop.viewport,
    marks: _noop.marks,
    lists: MainWindowListActions(onTrackSelected: onTrackSelected),
    mediaTimeline: _noop.mediaTimeline,
    deck: _noop.deck,
    analysisOverlay: _noop.analysisOverlay,
    overlays: _noop.overlays,
  );
}

MainWindowViewActions _noopWithOverlayActions({
  ValueChanged<double>? onMarksSidebarWidthChanged,
  VoidCallback? onCloseInspector,
}) {
  return MainWindowViewActions(
    drop: _noop.drop,
    toolbar: _noop.toolbar,
    viewport: _noop.viewport,
    marks: _noop.marks,
    mediaTimeline: _noop.mediaTimeline,
    deck: _noop.deck,
    analysisOverlay: _noop.analysisOverlay,
    overlays: MainWindowOverlayActions(
      onCloseMediaInfo: _noop.overlays.onCloseMediaInfo,
      onCloseProfiler: _noop.overlays.onCloseProfiler,
      onCloseSettings: _noop.overlays.onCloseSettings,
      onCloseMarksSidebar: _noop.overlays.onCloseMarksSidebar,
      onCloseInspector: onCloseInspector,
      onMarksSidebarWidthChanged:
          onMarksSidebarWidthChanged ??
          _noop.overlays.onMarksSidebarWidthChanged,
      onViewportPixelSizeModeChanged:
          _noop.overlays.onViewportPixelSizeModeChanged,
      onPerformanceAlertPolicyChanged:
          _noop.overlays.onPerformanceAlertPolicyChanged,
      onFullScreenPointerActivity: _noop.overlays.onFullScreenPointerActivity,
      onFullScreenControlsHoverChanged:
          _noop.overlays.onFullScreenControlsHoverChanged,
    ),
  );
}

class _FakeAnalysisToolbarDataSource extends ChangeNotifier
    implements AnalysisToolbarDataSource {
  @override
  String? get activeOverlayHash => null;

  @override
  Set<int> get activeOverlayTrackFileIds => const {};

  @override
  AnalysisOverlayConfig get overlayConfig => const AnalysisOverlayConfig();

  @override
  bool get overlayPanelVisible => false;

  @override
  AnalysisError? get error => null;

  @override
  AnalysisState get state => AnalysisState.idle;

  @override
  String formatBytes(int bytes) => '$bytes B';

  @override
  Future<Map<String, int>> currentBytesByHash(Set<String> hashes) async =>
      const {};

  @override
  Future<AnalysisCacheSnapshot> snapshot() async => const AnalysisCacheSnapshot(
    path: '',
    totalBytes: 0,
    indexedBytes: 0,
    unindexedBytes: 0,
    maxBytes: 0,
    entries: [],
  );

  @override
  bool supportsOverlayForHash(String hash) => false;

  @override
  AnalysisTrackGenerationStatus? statusForPath(String path) => null;
}
