import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_cache.dart';
import 'package:void_player/analysis/analysis_manager.dart';
import 'package:void_player/analysis/analysis_overlay.dart';
import 'package:void_player/analysis/analysis_toolbar_data_source.dart';
import 'package:void_player/feedback/app_feedback.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/preferences/playback_preferences.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/viewport/viewport_display_state.dart';
import 'package:void_player/widgets/media_header.dart';
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
}

Widget _localized(Widget child) => MaterialApp(
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  home: child,
);

MainWindowViewModel _model({required bool settingsVisible}) =>
    MainWindowViewModel(
      fullFrameCaptureKey: GlobalKey(),
      viewport: MainWindowViewportVm(
        viewMode: 0,
        viewModeEnabled: true,
        textureId: null,
        viewportState: const ViewportDisplayState.empty(),
        layout: const LayoutState(),
        viewportKey: GlobalKey(),
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
        fullScreen: false,
        fullScreenControlsVisible: false,
      ),
    );

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
  ),
  viewport: MainWindowViewportActions(
    onPan: (_) {},
    onSplit: (_) {},
    onZoom: (_, _) {},
    onPointerButton: (_, _) {},
    onResize: (_, _, _) {},
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
    onViewportPixelSizeModeChanged: (_) {},
    onPerformanceAlertPolicyChanged: (_) {},
    onFullScreenPointerActivity: () {},
    onFullScreenControlsHoverChanged: (_) {},
  ),
);

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
