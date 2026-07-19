import 'package:flutter/foundation.dart';

import '../analysis/ui/analysis_ui_selection.dart';
import '../marks/quick_mark.dart';
import '../marks/quick_mark_thumbnail.dart';
import '../preferences/playback_preferences.dart';
import '../video_renderer_controller.dart';
import '../viewport/viewport_display_state.dart';

const Object _mainWindowStateUnset = Object();
const double kDefaultTimelineControlsWidth = 332.0;
const double kDefaultMarksSidebarWidth = 340.0;
const double kMinMarksSidebarWidth = 260.0;
const double kMaxMarksSidebarWidth = 560.0;
const double kMarksSidebarResizeHandleWidth = 9.0;
const double kDefaultDeckHeight = 260.0;
const double kMinDeckHeight = 180.0;
const double kMaxDeckHeight = 440.0;

enum MainWindowDeckTab { timeline, analysis }

class MainWindowStateModel {
  final int? playerId;
  final int? textureId;
  final ViewportDisplayState viewportState;
  final bool isPlaying;
  final double playbackSpeed;
  final int currentPtsUs;
  final int durationUs;
  final LayoutState layout;
  final int? pendingSeekUs;
  final DateTime? pendingSeekAt;
  final Map<int, int> syncOffsets; // fileId -> offset in microseconds
  final double timelineControlsWidth;
  final MainWindowDeckTab deckTab;
  final double deckHeight;
  final bool deckCollapsed;
  final bool loopRangeEnabled;
  final bool nativeLoopRangeSynced;
  final bool startupLoopRangeApplied;
  final int loopStartUs;
  final int loopEndUs;
  final bool dragging;
  final bool mediaInfoVisible;
  final bool profilerVisible;
  final bool settingsVisible;
  final bool analysisOverlayControlsVisible;
  final bool marksSidebarVisible;
  final double marksSidebarWidth;
  final bool fullScreen;
  final bool fullScreenControlsVisible;
  final bool nativeCompositorActive;
  final int? audibleTrackFileId;
  final PerformanceAlertPolicy performanceAlertPolicy;
  final Map<int, QuickMarkAnchor> presentedFrameAnchors;
  final List<QuickMark> quickMarks;
  final Map<int, QuickMarkThumbnail> quickMarkThumbnails;
  final QuickMark? quickMarkDraft;
  final int? selectedQuickMarkId;
  final AnalysisUiSelection? analysisSelection;
  final int? selectedTrackFileId;

  const MainWindowStateModel({
    this.playerId,
    this.textureId,
    this.viewportState = const ViewportDisplayState.empty(),
    this.isPlaying = false,
    this.playbackSpeed = 1.0,
    this.currentPtsUs = 0,
    this.durationUs = 0,
    this.layout = const LayoutState(),
    this.pendingSeekUs,
    this.pendingSeekAt,
    this.syncOffsets = const {},
    this.timelineControlsWidth = kDefaultTimelineControlsWidth,
    this.deckTab = MainWindowDeckTab.timeline,
    this.deckHeight = kDefaultDeckHeight,
    this.deckCollapsed = false,
    this.loopRangeEnabled = false,
    this.nativeLoopRangeSynced = false,
    this.startupLoopRangeApplied = false,
    this.loopStartUs = 0,
    this.loopEndUs = 0,
    this.dragging = false,
    this.mediaInfoVisible = false,
    this.profilerVisible = false,
    this.settingsVisible = false,
    this.analysisOverlayControlsVisible = false,
    this.marksSidebarVisible = false,
    this.marksSidebarWidth = kDefaultMarksSidebarWidth,
    this.fullScreen = false,
    this.fullScreenControlsVisible = false,
    this.nativeCompositorActive = false,
    this.audibleTrackFileId,
    this.performanceAlertPolicy = PerformanceAlertPolicy.sustained,
    this.presentedFrameAnchors = const {},
    this.quickMarks = const [],
    this.quickMarkThumbnails = const {},
    this.quickMarkDraft,
    this.selectedQuickMarkId,
    this.analysisSelection,
    this.selectedTrackFileId,
  });

  MainWindowStateModel copyWith({
    Object? playerId = _mainWindowStateUnset,
    Object? textureId = _mainWindowStateUnset,
    ViewportDisplayState? viewportState,
    bool? isPlaying,
    double? playbackSpeed,
    int? currentPtsUs,
    int? durationUs,
    LayoutState? layout,
    Object? pendingSeekUs = _mainWindowStateUnset,
    Object? pendingSeekAt = _mainWindowStateUnset,
    Map<int, int>? syncOffsets,
    double? timelineControlsWidth,
    MainWindowDeckTab? deckTab,
    double? deckHeight,
    bool? deckCollapsed,
    bool? loopRangeEnabled,
    bool? nativeLoopRangeSynced,
    bool? startupLoopRangeApplied,
    int? loopStartUs,
    int? loopEndUs,
    bool? dragging,
    bool? mediaInfoVisible,
    bool? profilerVisible,
    bool? settingsVisible,
    bool? analysisOverlayControlsVisible,
    bool? marksSidebarVisible,
    double? marksSidebarWidth,
    bool? fullScreen,
    bool? fullScreenControlsVisible,
    bool? nativeCompositorActive,
    Object? audibleTrackFileId = _mainWindowStateUnset,
    PerformanceAlertPolicy? performanceAlertPolicy,
    Map<int, QuickMarkAnchor>? presentedFrameAnchors,
    List<QuickMark>? quickMarks,
    Map<int, QuickMarkThumbnail>? quickMarkThumbnails,
    Object? quickMarkDraft = _mainWindowStateUnset,
    Object? selectedQuickMarkId = _mainWindowStateUnset,
    Object? analysisSelection = _mainWindowStateUnset,
    Object? selectedTrackFileId = _mainWindowStateUnset,
  }) {
    return MainWindowStateModel(
      playerId: playerId == _mainWindowStateUnset
          ? this.playerId
          : playerId as int?,
      textureId: textureId == _mainWindowStateUnset
          ? this.textureId
          : textureId as int?,
      viewportState: viewportState ?? this.viewportState,
      isPlaying: isPlaying ?? this.isPlaying,
      playbackSpeed: playbackSpeed ?? this.playbackSpeed,
      currentPtsUs: currentPtsUs ?? this.currentPtsUs,
      durationUs: durationUs ?? this.durationUs,
      layout: layout ?? this.layout,
      pendingSeekUs: pendingSeekUs == _mainWindowStateUnset
          ? this.pendingSeekUs
          : pendingSeekUs as int?,
      pendingSeekAt: pendingSeekAt == _mainWindowStateUnset
          ? this.pendingSeekAt
          : pendingSeekAt as DateTime?,
      syncOffsets: syncOffsets ?? this.syncOffsets,
      timelineControlsWidth:
          timelineControlsWidth ?? this.timelineControlsWidth,
      deckTab: deckTab ?? this.deckTab,
      deckHeight: deckHeight ?? this.deckHeight,
      deckCollapsed: deckCollapsed ?? this.deckCollapsed,
      loopRangeEnabled: loopRangeEnabled ?? this.loopRangeEnabled,
      nativeLoopRangeSynced:
          nativeLoopRangeSynced ?? this.nativeLoopRangeSynced,
      startupLoopRangeApplied:
          startupLoopRangeApplied ?? this.startupLoopRangeApplied,
      loopStartUs: loopStartUs ?? this.loopStartUs,
      loopEndUs: loopEndUs ?? this.loopEndUs,
      dragging: dragging ?? this.dragging,
      mediaInfoVisible: mediaInfoVisible ?? this.mediaInfoVisible,
      profilerVisible: profilerVisible ?? this.profilerVisible,
      settingsVisible: settingsVisible ?? this.settingsVisible,
      analysisOverlayControlsVisible:
          analysisOverlayControlsVisible ?? this.analysisOverlayControlsVisible,
      marksSidebarVisible: marksSidebarVisible ?? this.marksSidebarVisible,
      marksSidebarWidth: marksSidebarWidth ?? this.marksSidebarWidth,
      fullScreen: fullScreen ?? this.fullScreen,
      fullScreenControlsVisible:
          fullScreenControlsVisible ?? this.fullScreenControlsVisible,
      nativeCompositorActive:
          nativeCompositorActive ?? this.nativeCompositorActive,
      audibleTrackFileId: audibleTrackFileId == _mainWindowStateUnset
          ? this.audibleTrackFileId
          : audibleTrackFileId as int?,
      performanceAlertPolicy:
          performanceAlertPolicy ?? this.performanceAlertPolicy,
      presentedFrameAnchors:
          presentedFrameAnchors ?? this.presentedFrameAnchors,
      quickMarks: quickMarks ?? this.quickMarks,
      quickMarkThumbnails: quickMarkThumbnails ?? this.quickMarkThumbnails,
      quickMarkDraft: quickMarkDraft == _mainWindowStateUnset
          ? this.quickMarkDraft
          : quickMarkDraft as QuickMark?,
      selectedQuickMarkId: selectedQuickMarkId == _mainWindowStateUnset
          ? this.selectedQuickMarkId
          : selectedQuickMarkId as int?,
      analysisSelection: analysisSelection == _mainWindowStateUnset
          ? this.analysisSelection
          : analysisSelection as AnalysisUiSelection?,
      selectedTrackFileId: selectedTrackFileId == _mainWindowStateUnset
          ? this.selectedTrackFileId
          : selectedTrackFileId as int?,
    );
  }
}

class MainWindowStateStore extends ChangeNotifier {
  MainWindowStateModel _value = const MainWindowStateModel();
  bool _disposed = false;

  MainWindowStateModel get value => _value;

  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }

  void _set(MainWindowStateModel next) {
    if (_disposed) return;
    _value = next;
    notifyListeners();
  }

  void setViewportState(ViewportDisplayState state) {
    if (_value.viewportState == state) return;
    _set(_value.copyWith(viewportState: state));
  }

  void setPlayerIdentity({required int playerId, int? textureId}) {
    if (_value.playerId == playerId && _value.textureId == textureId) return;
    _set(_value.copyWith(playerId: playerId, textureId: textureId));
  }

  void setLayout(LayoutState layout) {
    if (_value.layout == layout) return;
    _set(_value.copyWith(layout: layout));
  }

  void setSyncOffsets(Map<int, int> offsets) {
    if (mapEquals(_value.syncOffsets, offsets)) return;
    _set(_value.copyWith(syncOffsets: offsets));
  }

  void resetAfterLastTrackRemoved() {
    _set(
      _value.copyWith(
        playerId: null,
        textureId: null,
        viewportState: const ViewportDisplayState.empty(),
        isPlaying: false,
        currentPtsUs: 0,
        durationUs: 0,
        layout: const LayoutState(),
        syncOffsets: const {},
        presentedFrameAnchors: const {},
        quickMarks: const [],
        quickMarkThumbnails: const {},
        quickMarkDraft: null,
        selectedQuickMarkId: null,
        analysisSelection: null,
        selectedTrackFileId: null,
        loopRangeEnabled: false,
        nativeLoopRangeSynced: false,
        startupLoopRangeApplied: false,
        loopStartUs: 0,
        loopEndUs: 0,
        fullScreen: false,
        fullScreenControlsVisible: false,
        nativeCompositorActive: false,
        audibleTrackFileId: null,
        mediaInfoVisible: false,
        analysisOverlayControlsVisible: false,
        marksSidebarVisible: false,
      ),
    );
  }

  void setPlaying(bool playing) {
    if (_value.isPlaying == playing) return;
    _set(_value.copyWith(isPlaying: playing));
  }

  void setPlaybackSpeed(double speed) {
    if (_value.playbackSpeed == speed) return;
    _set(_value.copyWith(playbackSpeed: speed));
  }

  void setSeekPreview(
    int ptsUs, {
    Map<int, QuickMarkAnchor> presentedFrameAnchors = const {},
  }) {
    _set(
      _value.copyWith(
        currentPtsUs: ptsUs,
        pendingSeekUs: ptsUs,
        pendingSeekAt: DateTime.now(),
        presentedFrameAnchors: presentedFrameAnchors,
      ),
    );
  }

  void setPendingSeek(int? ptsUs, DateTime? at) {
    if (_value.pendingSeekUs == ptsUs && _value.pendingSeekAt == at) return;
    _set(_value.copyWith(pendingSeekUs: ptsUs, pendingSeekAt: at));
  }

  void setTimelineControlsWidth(double width) {
    if (_value.timelineControlsWidth == width) return;
    _set(_value.copyWith(timelineControlsWidth: width));
  }

  void setDeckTab(MainWindowDeckTab tab) {
    if (_value.deckTab == tab) return;
    _set(_value.copyWith(deckTab: tab));
  }

  void setDeckHeight(double height) {
    final next = height.clamp(kMinDeckHeight, kMaxDeckHeight).toDouble();
    if (_value.deckHeight == next) return;
    _set(_value.copyWith(deckHeight: next));
  }

  void setDeckCollapsed(bool collapsed) {
    if (_value.deckCollapsed == collapsed) return;
    _set(_value.copyWith(deckCollapsed: collapsed));
  }

  void setMarksSidebarWidth(double width) {
    final next = width
        .clamp(kMinMarksSidebarWidth, kMaxMarksSidebarWidth)
        .toDouble();
    if (_value.marksSidebarWidth == next) return;
    _set(_value.copyWith(marksSidebarWidth: next));
  }

  void setPolledPlaybackState(
    int ptsUs,
    int durationUs,
    bool playing, {
    Map<int, QuickMarkAnchor>? presentedFrameAnchors,
  }) {
    if (_value.currentPtsUs == ptsUs &&
        _value.durationUs == durationUs &&
        _value.isPlaying == playing &&
        (presentedFrameAnchors == null ||
            mapEquals(_value.presentedFrameAnchors, presentedFrameAnchors))) {
      return;
    }
    _set(
      _value.copyWith(
        currentPtsUs: ptsUs,
        durationUs: durationUs,
        isPlaying: playing,
        presentedFrameAnchors:
            presentedFrameAnchors ?? _value.presentedFrameAnchors,
      ),
    );
  }

  void setLoopRangeEnabled(bool enabled) {
    if (_value.loopRangeEnabled == enabled) return;
    _set(_value.copyWith(loopRangeEnabled: enabled));
  }

  void setNativeLoopRangeSynced(bool synced) {
    if (_value.nativeLoopRangeSynced == synced) return;
    _set(_value.copyWith(nativeLoopRangeSynced: synced));
  }

  void setStartupLoopRangeApplied(bool applied) {
    if (_value.startupLoopRangeApplied == applied) return;
    _set(_value.copyWith(startupLoopRangeApplied: applied));
  }

  void setLoopRange(int startUs, int endUs) {
    if (_value.loopStartUs == startUs && _value.loopEndUs == endUs) return;
    _set(_value.copyWith(loopStartUs: startUs, loopEndUs: endUs));
  }

  void setDragging(bool dragging) {
    if (_value.dragging == dragging) return;
    _set(_value.copyWith(dragging: dragging));
  }

  void setProfilerVisible(bool visible) {
    if (_value.profilerVisible == visible) return;
    _set(_value.copyWith(profilerVisible: visible));
  }

  void setMediaInfoVisible(bool visible) {
    if (_value.mediaInfoVisible == visible) return;
    _set(_value.copyWith(mediaInfoVisible: visible));
  }

  void setSettingsVisible(bool visible) {
    if (_value.settingsVisible == visible) return;
    _set(_value.copyWith(settingsVisible: visible));
  }

  void setAnalysisOverlayControlsVisible(bool visible) {
    if (_value.analysisOverlayControlsVisible == visible) return;
    _set(_value.copyWith(analysisOverlayControlsVisible: visible));
  }

  void setMarksSidebarVisible(bool visible) {
    if (_value.marksSidebarVisible == visible) return;
    _set(_value.copyWith(marksSidebarVisible: visible));
  }

  void setFullScreen(bool fullScreen) {
    if (_value.fullScreen == fullScreen &&
        _value.fullScreenControlsVisible == fullScreen) {
      return;
    }
    _set(
      _value.copyWith(
        fullScreen: fullScreen,
        fullScreenControlsVisible: fullScreen,
      ),
    );
  }

  void setFullScreenControlsVisible(bool visible) {
    if (_value.fullScreenControlsVisible == visible) return;
    _set(_value.copyWith(fullScreenControlsVisible: visible));
  }

  void setNativeCompositorActive(bool active) {
    if (_value.nativeCompositorActive == active) return;
    _set(_value.copyWith(nativeCompositorActive: active));
  }

  void setAudibleTrackFileId(int? fileId) {
    if (_value.audibleTrackFileId == fileId) return;
    _set(_value.copyWith(audibleTrackFileId: fileId));
  }

  void setPerformanceAlertPolicy(PerformanceAlertPolicy policy) {
    if (_value.performanceAlertPolicy == policy) return;
    _set(_value.copyWith(performanceAlertPolicy: policy));
  }

  void setQuickMarks(List<QuickMark> marks) {
    if (listEquals(_value.quickMarks, marks)) return;
    _set(_value.copyWith(quickMarks: List.unmodifiable(marks)));
  }

  void setQuickMarkThumbnails(Map<int, QuickMarkThumbnail> thumbnails) {
    if (mapEquals(_value.quickMarkThumbnails, thumbnails)) return;
    _set(_value.copyWith(quickMarkThumbnails: Map.unmodifiable(thumbnails)));
  }

  void setQuickMarkDraft(QuickMark? draft) {
    if (_value.quickMarkDraft == draft) return;
    _set(_value.copyWith(quickMarkDraft: draft));
  }

  void setSelectedQuickMarkId(int? id) {
    if (_value.selectedQuickMarkId == id &&
        (id == null ||
            (_value.analysisSelection == null &&
                _value.selectedTrackFileId == null))) {
      return;
    }
    _set(
      _value.copyWith(
        selectedQuickMarkId: id,
        analysisSelection: id == null ? _value.analysisSelection : null,
        selectedTrackFileId: id == null ? _value.selectedTrackFileId : null,
      ),
    );
  }

  void setAnalysisSelection(AnalysisUiSelection? selection) {
    if (_value.analysisSelection?.identity == selection?.identity &&
        (selection == null ||
            (_value.selectedQuickMarkId == null &&
                _value.selectedTrackFileId == null))) {
      return;
    }
    _set(
      _value.copyWith(
        selectedQuickMarkId: selection == null
            ? _value.selectedQuickMarkId
            : null,
        analysisSelection: selection,
        selectedTrackFileId: selection == null
            ? _value.selectedTrackFileId
            : null,
      ),
    );
  }

  void setSelectedTrackFileId(int? fileId) {
    if (_value.selectedTrackFileId == fileId &&
        (fileId == null ||
            (_value.selectedQuickMarkId == null &&
                _value.analysisSelection == null))) {
      return;
    }
    _set(
      _value.copyWith(
        selectedQuickMarkId: fileId == null ? _value.selectedQuickMarkId : null,
        analysisSelection: fileId == null ? _value.analysisSelection : null,
        selectedTrackFileId: fileId,
      ),
    );
  }
}

class TimelineHoverState {
  final int hoverPtsUs;
  final bool sliderHovering;

  const TimelineHoverState({this.hoverPtsUs = 0, this.sliderHovering = false});

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is TimelineHoverState &&
          other.hoverPtsUs == hoverPtsUs &&
          other.sliderHovering == sliderHovering;

  @override
  int get hashCode => Object.hash(hoverPtsUs, sliderHovering);
}
