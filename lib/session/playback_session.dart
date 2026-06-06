/// Playback-session constraints that can be applied by externally driven
/// review modes without teaching every widget about the mode name.
class PlaybackSession {
  final PlaybackSessionKind kind;
  final SessionCapabilities capabilities;
  final SessionRangeConstraint rangeConstraint;

  const PlaybackSession({
    required this.kind,
    required this.capabilities,
    this.rangeConstraint = const SessionRangeConstraint.unbounded(),
  });

  const PlaybackSession.normal()
    : kind = PlaybackSessionKind.normal,
      capabilities = const SessionCapabilities.allAllowed(),
      rangeConstraint = const SessionRangeConstraint.unbounded();
}

enum PlaybackSessionKind { normal, blindReview }

class SessionRangeConstraint {
  final int? startUs;
  final int? endUs;

  const SessionRangeConstraint({this.startUs, this.endUs});

  const SessionRangeConstraint.unbounded() : startUs = null, endUs = null;

  bool get isBounded => startUs != null || endUs != null;
}

class SessionCapabilities {
  final bool canOpenLocalMedia;
  final bool canOpenNetworkMedia;
  final bool canOpenSshMedia;
  final bool canAddTrack;
  final bool canRemoveTrack;
  final bool canReorderTrack;
  final bool canAdjustTrackOffset;
  final bool canToggleTrackAudio;
  final bool canChangeViewMode;
  final bool canPanViewport;
  final bool canZoomViewport;
  final bool canSeek;
  final bool canSeekOutsideRange;
  final bool canOpenMediaInfo;
  final bool canOpenProfiler;
  final bool canRunAnalysis;
  final bool canShowAnalysisOverlay;

  const SessionCapabilities({
    required this.canOpenLocalMedia,
    required this.canOpenNetworkMedia,
    required this.canOpenSshMedia,
    required this.canAddTrack,
    required this.canRemoveTrack,
    required this.canReorderTrack,
    required this.canAdjustTrackOffset,
    required this.canToggleTrackAudio,
    required this.canChangeViewMode,
    required this.canPanViewport,
    required this.canZoomViewport,
    required this.canSeek,
    required this.canSeekOutsideRange,
    required this.canOpenMediaInfo,
    required this.canOpenProfiler,
    required this.canRunAnalysis,
    required this.canShowAnalysisOverlay,
  });

  const SessionCapabilities.allAllowed()
    : canOpenLocalMedia = true,
      canOpenNetworkMedia = true,
      canOpenSshMedia = true,
      canAddTrack = true,
      canRemoveTrack = true,
      canReorderTrack = true,
      canAdjustTrackOffset = true,
      canToggleTrackAudio = true,
      canChangeViewMode = true,
      canPanViewport = true,
      canZoomViewport = true,
      canSeek = true,
      canSeekOutsideRange = true,
      canOpenMediaInfo = true,
      canOpenProfiler = true,
      canRunAnalysis = true,
      canShowAnalysisOverlay = true;
}
