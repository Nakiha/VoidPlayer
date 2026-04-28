import 'package:flutter/foundation.dart';

import '../video_renderer_controller.dart';

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
  final int hoverPtsUs;
  final bool sliderHovering;
  final bool dragging;

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
    this.hoverPtsUs = 0,
    this.sliderHovering = false,
    this.dragging = false,
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
    int? hoverPtsUs,
    bool? sliderHovering,
    bool? dragging,
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
      hoverPtsUs: hoverPtsUs ?? this.hoverPtsUs,
      sliderHovering: sliderHovering ?? this.sliderHovering,
      dragging: dragging ?? this.dragging,
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
    _set(_value.copyWith(viewportState: state));
  }

  void setTextureId(int textureId) {
    _set(_value.copyWith(textureId: textureId));
  }

  void setLayout(LayoutState layout) {
    _set(_value.copyWith(layout: layout));
  }

  void setSyncOffsets(Map<int, int> offsets) {
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
      ),
    );
  }

  void setPlaying(bool playing) {
    _set(_value.copyWith(isPlaying: playing));
  }

  void setPlaybackSpeed(double speed) {
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
    _set(_value.copyWith(pendingSeekUs: ptsUs, pendingSeekAt: at));
  }

  void setTimelineControlsWidth(double width) {
    if (_value.timelineControlsWidth == width) return;
    _set(_value.copyWith(timelineControlsWidth: width));
  }

  void setPolledPlaybackState(int ptsUs, int durationUs, bool playing) {
    _set(
      _value.copyWith(
        currentPtsUs: ptsUs,
        durationUs: durationUs,
        isPlaying: playing,
      ),
    );
  }

  void setLoopRangeEnabled(bool enabled) {
    _set(_value.copyWith(loopRangeEnabled: enabled));
  }

  void setNativeLoopRangeSynced(bool synced) {
    _set(_value.copyWith(nativeLoopRangeSynced: synced));
  }

  void setStartupLoopRangeApplied(bool applied) {
    _set(_value.copyWith(startupLoopRangeApplied: applied));
  }

  void setLoopRange(int startUs, int endUs) {
    _set(_value.copyWith(loopStartUs: startUs, loopEndUs: endUs));
  }

  void setSliderHover(int hoverUs, bool hovering) {
    _set(_value.copyWith(hoverPtsUs: hoverUs, sliderHovering: hovering));
  }

  void setDragging(bool dragging) {
    _set(_value.copyWith(dragging: dragging));
  }
}
