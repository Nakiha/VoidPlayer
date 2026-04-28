part of 'main_window.dart';

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
  final int viewportWidth;
  final int viewportHeight;
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
    this.viewportWidth = 0,
    this.viewportHeight = 0,
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
    int? viewportWidth,
    int? viewportHeight,
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
      viewportWidth: viewportWidth ?? this.viewportWidth,
      viewportHeight: viewportHeight ?? this.viewportHeight,
      dragging: dragging ?? this.dragging,
    );
  }
}
