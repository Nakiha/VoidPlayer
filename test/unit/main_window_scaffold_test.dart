import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_cache.dart';
import 'package:void_player/analysis/analysis_manager.dart';
import 'package:void_player/analysis/analysis_overlay.dart';
import 'package:void_player/analysis/analysis_toolbar_data_source.dart';
import 'package:void_player/feedback/app_feedback.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/preferences/playback_preferences.dart';
import 'package:void_player/session/playback_session.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/viewport/viewport_display_state.dart';
import 'package:void_player/widgets/media_header.dart';
import 'package:void_player/widgets/quick_mark_sidebar.dart';
import 'package:void_player/windows/main/main_window_overlays.dart';
import 'package:void_player/windows/main/main_window_scaffold.dart';
import 'package:void_player/windows/main/main_window_state.dart';
import 'package:void_player/windows/main/main_window_view_model.dart';

void main() {
  testWidgets('settings overlay is layered above media header panel host', (
    tester,
  ) async {
    final feedback = AppFeedbackController();
    addTearDown(feedback.dispose);

    await tester.pumpWidget(
      _localized(
        AppFeedbackScope(
          controller: feedback,
          child: MainWindowScaffold(
            model: _model(settingsVisible: false),
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
    final hostIndex = rootStack.children.indexWhere(
      (child) => child is MediaHeaderOverlayPanelHost,
    );
    final settingsIndex = rootStack.children.indexWhere(
      (child) => child is SettingsOverlaySlot,
    );

    expect(hostIndex, isNonNegative);
    expect(settingsIndex, isNonNegative);
    expect(settingsIndex, greaterThan(hostIndex));
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
  });

  testWidgets('marks sidebar row tap selects and jump button seeks', (
    tester,
  ) async {
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
            actions: _noopWithMarkActions(
              onJumpToMark: jumped.add,
              onSelectVisibleMark: selectedVisible.add,
            ),
          ),
        ),
      ),
    );

    expect(find.textContaining('Track 1'), findsWidgets);

    await tester.tap(find.byKey(const ValueKey('quick-mark-sidebar-row-1')));
    await tester.pump();

    expect(selectedVisible, equals([1]));
    expect(jumped, isEmpty);

    await tester.tap(find.byKey(const ValueKey('quick-mark-sidebar-jump-1')));
    await tester.pump();

    expect(jumped, equals([1]));
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
            actions: _noopWithOverlayActions(
              onMarksSidebarWidthChanged: widths.add,
            ),
          ),
        ),
      ),
    );

    final sidebar = find.byType(QuickMarkSidebar);
    final splitterX = tester.getTopLeft(sidebar).dx - 4;
    final splitterY = tester.getCenter(sidebar).dy;
    final gesture = await tester.startGesture(Offset(splitterX, splitterY));
    await gesture.moveBy(const Offset(-24, 0));
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

MainWindowViewModel _model({
  required bool settingsVisible,
  bool marksSidebarVisible = false,
  List<QuickMark> quickMarks = const [],
  int? selectedQuickMarkId,
  Map<int, TrackInfo> tracksByFileId = const {},
}) => MainWindowViewModel(
  fullFrameCaptureKey: GlobalKey(),
  session: MainWindowSessionVm.fromSession(const PlaybackSession.normal()),
  viewport: MainWindowViewportVm(
    viewMode: 0,
    viewModeEnabled: true,
    textureId: null,
    viewportState: const ViewportDisplayState.empty(),
    layout: const LayoutState(),
    viewportKey: GlobalKey(),
    tracks: const [],
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
    tracks: const [],
    syncOffsets: const {},
    audibleTrackFileId: null,
    performanceAlertPolicy: PerformanceAlertPolicy.sustained,
    analysisDataSource: _FakeAnalysisToolbarDataSource(),
    analysisOverlayButtonKey: GlobalKey(),
  ),
  playback: MainWindowPlaybackVm(
    timelineSliderKey: GlobalKey(),
    controlsBarKey: GlobalKey(),
    timelineStartWidth: 280,
    isPlaying: false,
    currentPtsUs: 0,
    durationUs: 0,
    markerUs: const [],
    seekMinUs: null,
    seekMaxUs: null,
    loopRangeBarKey: GlobalKey(),
    loopRangeEnabled: false,
    loopStartUs: 0,
    loopEndUs: 0,
    timelineHoverListenable: ValueNotifier(const TimelineHoverState()),
    controlsWidth: 280,
  ),
  overlays: MainWindowOverlayVm(
    dragging: false,
    mediaInfoVisible: false,
    profilerVisible: false,
    settingsVisible: settingsVisible,
    marksSidebarVisible: marksSidebarVisible,
    marksSidebarWidth: kDefaultMarksSidebarWidth,
    fullScreen: false,
    fullScreenControlsVisible: false,
  ),
);

QuickMark _quickMark(int id, Rect rect) {
  return QuickMark(
    id: id,
    anchor: const QuickMarkAnchor(fileId: 1, ptsUs: 0, dtsUs: 0),
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
    onQuickMarkStart: (_) {},
    onQuickMarkUpdate: (_) {},
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
    onRemoveTrack: (_) {},
    onZoomChanged: (_) {},
    onToggleFullScreen: () {},
    onTogglePlay: () {},
    onStepForward: () {},
    onStepBackward: () {},
    onSeek: (_) {},
    onSliderHover: (_, _) {},
    onLoopRangeEnabledChanged: (_) {},
    onLoopRangeChanged: (_, _) {},
    onLoopRangeChangeEnd: null,
    onReorder: (_, _) {},
    onOffsetChanged: (_, _) {},
    onToggleTrackAudio: (_) {},
    onControlsWidthChanged: (_) {},
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
    analysisOverlay: _noop.analysisOverlay,
    overlays: _noop.overlays,
  );
}

MainWindowViewActions _noopWithOverlayActions({
  ValueChanged<double>? onMarksSidebarWidthChanged,
}) {
  return MainWindowViewActions(
    drop: _noop.drop,
    toolbar: _noop.toolbar,
    viewport: _noop.viewport,
    marks: _noop.marks,
    mediaTimeline: _noop.mediaTimeline,
    analysisOverlay: _noop.analysisOverlay,
    overlays: MainWindowOverlayActions(
      onCloseMediaInfo: _noop.overlays.onCloseMediaInfo,
      onCloseProfiler: _noop.overlays.onCloseProfiler,
      onCloseSettings: _noop.overlays.onCloseSettings,
      onCloseMarksSidebar: _noop.overlays.onCloseMarksSidebar,
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
