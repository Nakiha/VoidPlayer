import 'package:flutter/foundation.dart';

import '../../video_renderer_controller.dart';

const Object _mainWindowStateUnset = Object();

class MainWindowStateModel {
  final int? textureId;
  final int viewportState;
  final bool isPlaying;
  final double playbackSpeed;
  final int currentPtsUs;
  final int durationUs;
  final LayoutState layout;
  final int? pendingSeekUs;
  final DateTime? pendingSeekAt;
  final Map<int, int> syncOffsets;
  final double timelineControlsWidth;
  final bool loopRangeEnabled;
  final bool nativeLoopRangeSynced;
  final bool startupLoopRangeApplied;
  final int loopStartUs;
  final int loopEndUs;
  final bool dragging;
  final bool profilerVisible;
  final bool settingsVisible;
  final bool fullScreen;
  final bool fullScreenControlsVisible;
  final int? audibleTrackFileId;

  const MainWindowStateModel({
    this.textureId,
    this.viewportState = 1,
    this.isPlaying = false,
    this.playbackSpeed = 1.0,
    this.currentPtsUs = 0,
    this.durationUs = 0,
    this.layout = const LayoutState(),
    this.pendingSeekUs,
    this.pendingSeekAt,
    this.syncOffsets = const {},
    this.timelineControlsWidth = 320,
    this.loopRangeEnabled = false,
    this.nativeLoopRangeSynced = false,
    this.startupLoopRangeApplied = false,
    this.loopStartUs = 0,
    this.loopEndUs = 0,
    this.dragging = false,
    this.profilerVisible = false,
    this.settingsVisible = false,
    this.fullScreen = false,
    this.fullScreenControlsVisible = false,
    this.audibleTrackFileId,
  });

  MainWindowStateModel copyWith({
    Object? textureId = _mainWindowStateUnset,
    int? viewportState,
    bool? isPlaying,
    double? playbackSpeed,
    int? currentPtsUs,
    int? durationUs,
    LayoutState? layout,
    Object? pendingSeekUs = _mainWindowStateUnset,
    Object? pendingSeekAt = _mainWindowStateUnset,
    Map<int, int>? syncOffsets,
    double? timelineControlsWidth,
    bool? loopRangeEnabled,
    bool? nativeLoopRangeSynced,
    bool? startupLoopRangeApplied,
    int? loopStartUs,
    int? loopEndUs,
    bool? dragging,
    bool? profilerVisible,
    bool? settingsVisible,
    bool? fullScreen,
    bool? fullScreenControlsVisible,
    Object? audibleTrackFileId = _mainWindowStateUnset,
  }) {
    return MainWindowStateModel(
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
      loopRangeEnabled: loopRangeEnabled ?? this.loopRangeEnabled,
      nativeLoopRangeSynced:
          nativeLoopRangeSynced ?? this.nativeLoopRangeSynced,
      startupLoopRangeApplied:
          startupLoopRangeApplied ?? this.startupLoopRangeApplied,
      loopStartUs: loopStartUs ?? this.loopStartUs,
      loopEndUs: loopEndUs ?? this.loopEndUs,
      dragging: dragging ?? this.dragging,
      profilerVisible: profilerVisible ?? this.profilerVisible,
      settingsVisible: settingsVisible ?? this.settingsVisible,
      fullScreen: fullScreen ?? this.fullScreen,
      fullScreenControlsVisible:
          fullScreenControlsVisible ?? this.fullScreenControlsVisible,
      audibleTrackFileId: audibleTrackFileId == _mainWindowStateUnset
          ? this.audibleTrackFileId
          : audibleTrackFileId as int?,
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

  void setViewportState(int state) {
    if (_value.viewportState == state) return;
    _set(_value.copyWith(viewportState: state));
  }

  void setTextureId(int textureId) {
    if (_value.textureId == textureId) return;
    _set(_value.copyWith(textureId: textureId));
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
        textureId: null,
        viewportState: 1,
        isPlaying: false,
        currentPtsUs: 0,
        durationUs: 0,
        layout: const LayoutState(),
        syncOffsets: const {},
        loopRangeEnabled: false,
        nativeLoopRangeSynced: false,
        loopStartUs: 0,
        loopEndUs: 0,
        fullScreen: false,
        fullScreenControlsVisible: false,
        audibleTrackFileId: null,
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

  void setSeekPreview(int ptsUs) {
    _set(
      _value.copyWith(
        currentPtsUs: ptsUs,
        pendingSeekUs: ptsUs,
        pendingSeekAt: DateTime.now(),
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

  void setPolledPlaybackState(int ptsUs, int durationUs, bool playing) {
    if (_value.currentPtsUs == ptsUs &&
        _value.durationUs == durationUs &&
        _value.isPlaying == playing) {
      return;
    }
    _set(
      _value.copyWith(
        currentPtsUs: ptsUs,
        durationUs: durationUs,
        isPlaying: playing,
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

  void setSettingsVisible(bool visible) {
    if (_value.settingsVisible == visible) return;
    _set(_value.copyWith(settingsVisible: visible));
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

  void setAudibleTrackFileId(int? fileId) {
    if (_value.audibleTrackFileId == fileId) return;
    _set(_value.copyWith(audibleTrackFileId: fileId));
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
