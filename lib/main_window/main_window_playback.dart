import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import '../app_log.dart';
import '../marks/quick_mark.dart';
import '../preferences/playback_preferences.dart';
import '../startup_options.dart';
import '../track_manager.dart';
import '../utils/async_guard.dart';
import '../video_renderer_controller.dart';
import '../viewport/viewport_display_state.dart';
import 'main_window_state.dart';
import 'main_window_timeline_metrics.dart';

String _quickMarkAnchorTrace(QuickMarkAnchor anchor) =>
    'pts=${anchor.ptsUs},dts=${anchor.dtsUs},dur=${anchor.durationUs},'
    'afi=${anchor.analysisFrameIndex},mode=${anchor.frameIdentityMode},'
    'spi=${anchor.sourcePacketIndex},sps=${anchor.sourcePacketSize},'
    'spp=${anchor.sourcePacketPos}';

const int _playbackClockFallbackAnchorDurationUs = 33334;
const int _presentedFrameAnchorToleranceUs = 50000;
const int _maxPresentedFrameDurationUs = 250000;
const Duration _pendingSeekTimeout = Duration(milliseconds: 1500);

class MainWindowPlaybackCoordinator {
  static const double trackDragHandleWidth = 28.0;
  static const double trackDividerWidth = 1.0;
  static const Duration _maxLoopBoundaryDelay = Duration(seconds: 30);

  final NativePlayerController controller;
  final TrackManager trackManager;
  final StartupOptions startupOptions;
  final MainWindowStateStore stateStore;
  final ValueNotifier<TimelineHoverState> timelineHoverNotifier;
  final PlaybackPreferences playbackPreferences;
  final bool Function() mounted;
  final MainWindowTimelineMetrics timelineMetrics;
  final Future<void> Function(int ptsUs)? onSeekSettled;
  final void Function({required int requestId})? onSeekStarted;
  final Future<void> Function({
    required int requestId,
    required int trackFileId,
    required int ptsUs,
    required int dtsUs,
  })?
  onSeekPreviewPresented;

  /// Invoked around play/pause/step transitions so the layout coordinator can
  /// flush deferred interaction state (compositor pan transform, pending
  /// layout) before playback content changes. Awaited before play starts so
  /// the first playing frame already carries the interacted layout.
  final Future<void> Function({required bool playing})? onPlaybackTransition;
  final void Function({required bool active})?
  onNativeCompositorAvailabilityChanged;

  Timer? _loopBoundaryTimer;
  Timer? _seekSettledTimer;
  Ticker? _playbackClockTicker;
  int? _playbackClockTickerBaseUs;
  _PlaybackClockAnchor? _playbackClockAnchor;
  StreamSubscription<NativePlayerEvent>? _nativeEventSubscription;
  bool _disposed = false;
  bool _resumeAfterSeek = false;
  int _pollSerial = 0;
  int _seekSerial = 0;
  int _seekPreviewEventSerial = 0;
  int _loopRangeSyncSerial = 0;
  String _lastQuickMarkAnchorTrace = '';

  MainWindowPlaybackCoordinator({
    required this.controller,
    required this.trackManager,
    required this.startupOptions,
    required this.stateStore,
    required this.timelineHoverNotifier,
    required this.playbackPreferences,
    required this.mounted,
    required this.timelineMetrics,
    this.onSeekSettled,
    this.onSeekStarted,
    this.onSeekPreviewPresented,
    this.onPlaybackTransition,
    this.onNativeCompositorAvailabilityChanged,
  }) {
    _nativeEventSubscription = controller.events.listen(
      _handleNativePlayerEvent,
      onError: (Object error, StackTrace stack) {
        log.warning('Native player event stream failed', error, stack);
      },
    );
  }

  MainWindowStateModel get _state => stateStore.value;

  int? playerId() => _state.playerId;
  int? textureId() => _state.textureId;
  double timelineControlsWidth() => _state.timelineControlsWidth;
  bool isPlaying() => _state.isPlaying;
  void setPlaying(bool playing) => stateStore.setPlaying(playing);
  double playbackSpeed() => _state.playbackSpeed;
  void setPlaybackSpeed(double speed) => stateStore.setPlaybackSpeed(speed);
  int currentPtsUs() => _state.currentPtsUs;
  int durationUs() => _state.durationUs;
  int? pendingSeekUs() => _state.pendingSeekUs;
  DateTime? pendingSeekAt() => _state.pendingSeekAt;
  void setSeekPreview(int ptsUs) => stateStore.setSeekPreview(ptsUs);
  void setPendingSeek(int? ptsUs, DateTime? at) =>
      stateStore.setPendingSeek(ptsUs, at);
  void setPolledPlaybackState(
    int ptsUs,
    int durationUs,
    bool playing, {
    Map<int, QuickMarkAnchor>? presentedFrameAnchors,
  }) => stateStore.setPolledPlaybackState(
    ptsUs,
    durationUs,
    playing,
    presentedFrameAnchors: presentedFrameAnchors,
  );
  bool loopRangeEnabled() => _state.loopRangeEnabled;
  void setLoopRangeEnabledState(bool enabled) =>
      stateStore.setLoopRangeEnabled(enabled);
  bool nativeLoopRangeSynced() => _state.nativeLoopRangeSynced;
  void setNativeLoopRangeSynced(bool synced) =>
      stateStore.setNativeLoopRangeSynced(synced);
  bool startupLoopRangeApplied() => _state.startupLoopRangeApplied;
  void setStartupLoopRangeApplied(bool applied) =>
      stateStore.setStartupLoopRangeApplied(applied);
  int loopStartUs() => _state.loopStartUs;
  int loopEndUs() => _state.loopEndUs;
  void setLoopRangeState(int startUs, int endUs) =>
      stateStore.setLoopRange(startUs, endUs);
  int hoverPtsUs() => timelineHoverNotifier.value.hoverPtsUs;
  bool sliderHovering() => timelineHoverNotifier.value.sliderHovering;
  void setSliderHoverState(int hoverUs, bool hovering) {
    final next = TimelineHoverState(
      hoverPtsUs: hoverUs,
      sliderHovering: hovering,
    );
    if (timelineHoverNotifier.value == next) return;
    timelineHoverNotifier.value = next;
  }

  void dispose() {
    _disposed = true;
    _pollSerial++;
    _seekPreviewEventSerial++;
    _loopRangeSyncSerial++;
    _playbackClockTickerBaseUs = null;
    _playbackClockTicker?.dispose();
    _playbackClockTicker = null;
    _loopBoundaryTimer?.cancel();
    _seekSettledTimer?.cancel();
    final nativeEventSubscription = _nativeEventSubscription;
    if (nativeEventSubscription != null) {
      fireAndLogFine(
        'cancel native player event subscription',
        nativeEventSubscription.cancel(),
      );
    }
    _nativeEventSubscription = null;
  }

  void startPolling() {
    if (_disposed) return;
    unawaited(_pollState());
  }

  void invalidateLoopRangeSync() {
    _loopRangeSyncSerial++;
  }

  Future<void> togglePlayPause() async {
    if (_disposed) return;
    if (isPlaying()) {
      await pause();
    } else {
      await play();
    }
  }

  void toggleTrackAudio(int fileId) {
    final next = _state.audibleTrackFileId == fileId ? null : fileId;
    stateStore.setAudibleTrackFileId(next);
    fireAndLog('set audible track', controller.setAudibleTrack(next));
  }

  Future<void> play() async {
    if (_disposed) return;
    _resumeAfterSeek = false;
    if (loopRangeEnabled() && !_currentPtsInsideLoopRange) {
      await _seekToAsync(resolvedLoopStartUs);
      if (_disposed || !mounted()) return;
    }
    await onPlaybackTransition?.call(playing: true);
    if (_disposed || !mounted()) return;
    await controller.play();
    if (_disposed || !mounted()) return;
    setPlaying(true);
    _setPlaybackClockAnchor(
      _PlaybackClockAnchor(
        ptsUs: currentPtsUs(),
        durationUs: timelineMetrics.effectiveDurationUs,
        playing: true,
        speed: playbackSpeed() <= 0 ? 1.0 : playbackSpeed(),
      ),
    );
    scheduleLoopBoundaryTimer();
  }

  Future<void> pause() async {
    if (_disposed) return;
    _resumeAfterSeek = false;
    _seekSerial++;
    _seekPreviewEventSerial++;
    await controller.pause();
    if (_disposed || !mounted()) return;
    cancelLoopBoundaryTimer();
    setPlaying(false);
    _setPlaybackClockAnchor(
      _PlaybackClockAnchor(
        ptsUs: currentPtsUs(),
        durationUs: timelineMetrics.effectiveDurationUs,
        playing: false,
        speed: playbackSpeed() <= 0 ? 1.0 : playbackSpeed(),
      ),
    );
    await onPlaybackTransition?.call(playing: false);
  }

  Future<void> stepForward() {
    return _step(forward: true);
  }

  Future<void> stepBackward() {
    return _step(forward: false);
  }

  Future<void> _step({required bool forward}) async {
    if (_disposed) return;
    _resumeAfterSeek = false;
    _seekSerial++;
    _pollSerial++;
    cancelLoopBoundaryTimer();
    await onPlaybackTransition?.call(playing: false);
    if (_disposed || !mounted()) return;
    if (forward) {
      await controller.stepForward();
    } else {
      await controller.stepBackward();
    }
    if (_disposed || !mounted()) return;
    setPlaying(false);

    int steppedPtsUs;
    try {
      steppedPtsUs = await controller.currentPts();
    } catch (error, stack) {
      log.warning(
        'step playback current PTS fallback: forward=$forward',
        error,
        stack,
      );
      steppedPtsUs = currentPtsUs();
    }
    if (_disposed || !mounted()) return;

    if (loopRangeEnabled() && resolvedLoopEndUs > resolvedLoopStartUs) {
      final startUs = resolvedLoopStartUs;
      final endUs = resolvedLoopEndUs;
      if ((forward && steppedPtsUs >= endUs) ||
          (!forward && steppedPtsUs < startUs)) {
        await _seekToAsync(startUs);
        return;
      }
    }

    setPolledPlaybackState(steppedPtsUs, durationUs(), false);
    _setPlaybackClockAnchor(
      _PlaybackClockAnchor(
        ptsUs: steppedPtsUs,
        durationUs: durationUs(),
        playing: false,
        speed: playbackSpeed() <= 0 ? 1.0 : playbackSpeed(),
      ),
    );
  }

  void setSpeed(double speed) {
    if (_disposed) return;
    final clamped = speed > 0 ? speed : 1.0;
    setPlaybackSpeed(clamped);
    final anchor = _playbackClockAnchor;
    if (anchor != null) {
      _setPlaybackClockAnchor(
        _PlaybackClockAnchor(
          ptsUs: currentPtsUs(),
          durationUs: anchor.durationUs,
          playing: anchor.playing,
          speed: clamped,
        ),
      );
    }
    fireAndLog('set playback speed', controller.setSpeed(clamped));
    scheduleLoopBoundaryTimer();
  }

  void seekTo(int ptsUs, {bool preservePresentedFrameAnchors = false}) {
    if (_disposed) return;
    unawaited(
      _seekToAsync(
        ptsUs,
        preservePresentedFrameAnchors: preservePresentedFrameAnchors,
      ).catchError((Object error, StackTrace stack) {
        log.warning('seek failed: targetPtsUs=$ptsUs', error, stack);
      }),
    );
  }

  Future<void> seekToAndWait(int ptsUs) {
    if (_disposed) return Future<void>.value();
    return _seekToAsync(ptsUs);
  }

  Future<void> _seekToAsync(
    int ptsUs, {
    bool preservePresentedFrameAnchors = false,
  }) async {
    if (_disposed) return;
    final targetPtsUs = _clampSeekTargetUs(ptsUs);
    final seekSerial = ++_seekSerial;
    onSeekStarted?.call(requestId: seekSerial);
    _seekPreviewEventSerial++;
    _pollSerial++;
    final wasPlaying = isPlaying();
    final behavior = playbackPreferences.seekAfterJumpBehavior;
    final shouldResume =
        behavior == SeekAfterJumpBehavior.keepPreviousState &&
        (wasPlaying || _resumeAfterSeek);
    stateStore.setSeekPreview(
      targetPtsUs,
      presentedFrameAnchors: preservePresentedFrameAnchors
          ? _state.presentedFrameAnchors
          : const {},
    );
    _setPlaybackClockAnchor(
      _PlaybackClockAnchor(
        ptsUs: targetPtsUs,
        durationUs: timelineMetrics.effectiveDurationUs,
        playing: false,
        speed: playbackSpeed() <= 0 ? 1.0 : playbackSpeed(),
      ),
    );
    if (wasPlaying) {
      _resumeAfterSeek = behavior == SeekAfterJumpBehavior.keepPreviousState;
      cancelLoopBoundaryTimer();
      await controller.pause();
      if (_disposed || !mounted()) return;
      setPlaying(false);
      _setPlaybackClockAnchor(
        _PlaybackClockAnchor(
          ptsUs: targetPtsUs,
          durationUs: timelineMetrics.effectiveDurationUs,
          playing: false,
          speed: playbackSpeed() <= 0 ? 1.0 : playbackSpeed(),
        ),
      );
    }
    await controller.seek(targetPtsUs, requestId: seekSerial);
    if (_disposed || !mounted()) return;

    if (seekSerial != _seekSerial) return;
    _schedulePendingSeekTimeout(seekSerial, targetPtsUs);

    final resume = shouldResume && _resumeAfterSeek;
    _resumeAfterSeek = false;
    if (resume) {
      await controller.play();
      if (_disposed || !mounted() || seekSerial != _seekSerial) return;
      setPlaying(true);
      _setPlaybackClockAnchor(
        _PlaybackClockAnchor(
          ptsUs: targetPtsUs,
          durationUs: timelineMetrics.effectiveDurationUs,
          playing: true,
          speed: playbackSpeed() <= 0 ? 1.0 : playbackSpeed(),
        ),
      );
    }
    scheduleLoopBoundaryTimer(fromPtsUs: targetPtsUs);
  }

  void _schedulePendingSeekTimeout(int seekSerial, int targetPtsUs) {
    _seekSettledTimer?.cancel();
    _seekSettledTimer = Timer(_pendingSeekTimeout, () {
      if (_disposed || !mounted() || seekSerial != _seekSerial) return;
      _completePendingSeekByTimeout(
        seekSerial: seekSerial,
        targetPtsUs: targetPtsUs,
      );
    });
  }

  void _completePendingSeekByTimeout({
    required int seekSerial,
    required int targetPtsUs,
  }) {
    if (_disposed ||
        !mounted() ||
        seekSerial != _seekSerial ||
        pendingSeekUs() != targetPtsUs) {
      return;
    }
    logFine(
      'Seek preview event timed out; committing target fallback '
      '(requestId=$seekSerial, targetPtsUs=$targetPtsUs)',
    );
    _seekSettledTimer?.cancel();
    _seekSettledTimer = null;
    _clearPendingSeek();
    final presentedFrameAnchors = _needsPresentedFrameAnchors
        ? _fallbackPresentedFrameAnchorsForTimeline(targetPtsUs)
        : null;
    setPolledPlaybackState(
      targetPtsUs,
      durationUs(),
      isPlaying(),
      presentedFrameAnchors: presentedFrameAnchors,
    );
    _setPlaybackClockAnchor(
      _PlaybackClockAnchor(
        ptsUs: targetPtsUs,
        durationUs: durationUs(),
        playing: isPlaying(),
        speed: playbackSpeed() <= 0 ? 1.0 : playbackSpeed(),
      ),
    );
    final seekSettled = onSeekSettled;
    if (seekSettled == null) return;
    unawaited(
      seekSettled(targetPtsUs).catchError((Object error, StackTrace stack) {
        log.warning(
          'seek settled fallback failed: targetPtsUs=$targetPtsUs',
          error,
          stack,
        );
      }),
    );
  }

  void _handleNativePlayerEvent(NativePlayerEvent event) {
    if (_disposed || !mounted()) return;
    if (event.type == NativePlayerEventType.nativeCompositorState) {
      log.info(
        'Native compositor state: phase=${event.nativeCompositorPhase} '
        'serial=${event.nativeCompositorSerial} '
        'active=${event.nativeCompositorActive} '
        'reason=${event.nativeCompositorReason}',
      );
      final wasActive = stateStore.value.nativeCompositorActive;
      stateStore.setNativeCompositorActive(event.nativeCompositorActive);
      if (wasActive != event.nativeCompositorActive) {
        onNativeCompositorAvailabilityChanged?.call(
          active: event.nativeCompositorActive,
        );
      }
      if (Platform.isWindows &&
          event.nativeCompositorSerial > 0 &&
          event.nativeCompositorPhase == 'preparing') {
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (_disposed || !mounted()) return;
          log.info(
            'Native compositor Flutter-state ACK: '
            'serial=${event.nativeCompositorSerial} '
            'transparent=true',
          );
          unawaited(
            () async {
              await controller.ackNativeCompositorFlutterState(
                serial: event.nativeCompositorSerial,
                transparentViewport: true,
              );
            }().catchError((Object error, StackTrace stack) {
              log.warning(
                'native compositor Flutter-state ACK failed',
                error,
                stack,
              );
            }),
          );
        });
      }
      if (event.nativeCompositorRequested &&
          !event.nativeCompositorActive &&
          event.nativeCompositorFailure.isNotEmpty) {
        log.warning(
          'Native compositor unavailable; Flutter texture fallback is disabled: '
          '${event.nativeCompositorFailure}',
        );
        stateStore.setViewportState(
          ViewportDisplayState.error(
            'Native compositor failed: '
            '${event.nativeCompositorFailure}',
          ),
        );
      }
      return;
    }
    if (event.type == NativePlayerEventType.playbackClock) {
      _handlePlaybackClockEvent(event);
      return;
    }
    if (event.type != NativePlayerEventType.seekPreviewPresented) return;
    unawaited(_handleSeekPreviewPresentedEvent(event));
  }

  Future<void> _handleSeekPreviewPresentedEvent(NativePlayerEvent event) async {
    final requestId = event.requestId;
    if (requestId == null || requestId != _seekSerial) return;
    if (!event.hasPresentedFrame) return;
    final seekUs = pendingSeekUs();
    if (seekUs == null) return;
    final targetPtsUs = event.targetPtsUs ?? seekUs;
    if ((targetPtsUs - seekUs).abs() > 50000) return;
    final eventSerial = ++_seekPreviewEventSerial;
    final eventPtsUs = event.ptsUs!;
    final eventDtsUs = event.dtsUs!;
    Map<int, QuickMarkAnchor>? presentedFrameAnchors;
    if (_needsPresentedFrameAnchors) {
      final timings = await _pollPresentedFrameTimings();
      if (_disposed ||
          !mounted() ||
          requestId != _seekSerial ||
          eventSerial != _seekPreviewEventSerial ||
          pendingSeekUs() != seekUs) {
        return;
      }
      presentedFrameAnchors = {
        for (final entry in trackManager.entries)
          entry.info.fileId: _quickMarkAnchorFromPresentedFrame(
            fileId: entry.info.fileId,
            timing: timings[entry.info.fileId],
            fallbackPtsUs: _trackPtsForTimelinePts(
              entry.info.fileId,
              targetPtsUs,
            ),
            fallbackDtsUs: _trackPtsForTimelinePts(
              entry.info.fileId,
              targetPtsUs,
            ),
            fallbackDurationUs: _playbackClockFallbackAnchorDurationUs,
            expectedPtsUs: _trackPtsForTimelinePts(
              entry.info.fileId,
              targetPtsUs,
            ),
            requireExpectedPtsMatch: true,
          )!,
      };
    } else if (_disposed ||
        !mounted() ||
        requestId != _seekSerial ||
        eventSerial != _seekPreviewEventSerial ||
        pendingSeekUs() != seekUs) {
      return;
    }
    _clearPendingSeek();
    final playing = isPlaying();
    setPolledPlaybackState(
      targetPtsUs,
      durationUs(),
      playing,
      presentedFrameAnchors: presentedFrameAnchors,
    );
    _setPlaybackClockAnchor(
      _PlaybackClockAnchor(
        ptsUs: targetPtsUs,
        durationUs: durationUs(),
        playing: playing,
        speed: playbackSpeed() <= 0 ? 1.0 : playbackSpeed(),
      ),
    );
    _seekSettledTimer?.cancel();
    _seekSettledTimer = null;
    final callback = onSeekPreviewPresented;
    if (callback == null) return;
    unawaited(
      callback(
        requestId: requestId,
        trackFileId: event.trackFileId!,
        ptsUs: eventPtsUs,
        dtsUs: eventDtsUs,
      ).catchError((Object error, StackTrace stack) {
        log.warning(
          'seek preview callback failed: requestId=$requestId',
          error,
          stack,
        );
      }),
    );
  }

  void _handlePlaybackClockEvent(NativePlayerEvent event) {
    final eventPtsUs = event.ptsUs;
    final eventDurationUs = event.durationUs;
    if (eventPtsUs == null || eventDurationUs == null) return;

    final seekUs = pendingSeekUs();
    var pts = eventPtsUs;
    if (seekUs != null) {
      final seekAge = pendingSeekAt() == null
          ? Duration.zero
          : DateTime.now().difference(pendingSeekAt()!);
      const seekSettleToleranceUs = 50000;
      final settled = (pts - seekUs).abs() <= seekSettleToleranceUs;
      if (_needsPresentedFrameAnchors) {
        if (seekAge < _pendingSeekTimeout) {
          pts = seekUs;
        } else {
          _clearPendingSeek();
          pts = seekUs;
        }
      } else if (settled) {
        _clearPendingSeek();
      } else if (seekAge < _pendingSeekTimeout) {
        pts = seekUs;
      } else {
        _clearPendingSeek();
      }
    }

    _applyPlaybackClockState(
      ptsUs: pts,
      durationUs: eventDurationUs,
      playing: event.isPlaying,
      speed: event.playbackSpeed,
      fromNativeEvent: true,
    );
  }

  void _applyPlaybackClockState({
    required int ptsUs,
    required int durationUs,
    required bool playing,
    required double speed,
    required bool fromNativeEvent,
  }) {
    final clampedPts = durationUs > 0
        ? ptsUs.clamp(0, durationUs).toInt()
        : ptsUs;
    if (loopRangeEnabled() &&
        playing &&
        pendingSeekUs() == null &&
        resolvedLoopEndUs > resolvedLoopStartUs &&
        clampedPts >= resolvedLoopEndUs) {
      seekTo(resolvedLoopStartUs);
      return;
    }

    if (fromNativeEvent) {
      _setPlaybackClockAnchor(
        _PlaybackClockAnchor(
          ptsUs: clampedPts,
          durationUs: durationUs,
          playing: playing,
          speed: speed <= 0 ? 1.0 : speed,
        ),
      );
    }

    final presentedFrameAnchors =
        _needsPresentedFrameAnchors && pendingSeekUs() == null
        ? _fallbackPresentedFrameAnchorsForTimeline(
            clampedPts,
            durationUs: _playbackClockFallbackAnchorDurationUs,
          )
        : null;
    setPolledPlaybackState(
      clampedPts,
      durationUs,
      playing,
      presentedFrameAnchors: presentedFrameAnchors,
    );
    if (fromNativeEvent) {
      if (playing) {
        scheduleLoopBoundaryTimer(fromPtsUs: clampedPts);
      } else {
        cancelLoopBoundaryTimer();
      }
    }
  }

  void _setPlaybackClockAnchor(_PlaybackClockAnchor anchor) {
    _playbackClockAnchor = anchor;
    _playbackClockTickerBaseUs = null;
    _syncPlaybackClockTicker();
  }

  void _syncPlaybackClockTicker() {
    final shouldTick = !_disposed && (_playbackClockAnchor?.playing ?? false);
    if (!shouldTick) {
      _playbackClockTicker?.stop();
      _playbackClockTickerBaseUs = null;
      return;
    }
    final ticker = _playbackClockTicker ??= Ticker(_onPlaybackClockTick);
    if (!ticker.isActive) {
      ticker.start();
    }
  }

  void _onPlaybackClockTick(Duration elapsed) {
    if (_disposed || !mounted()) return;
    final anchor = _playbackClockAnchor;
    if (anchor == null || !anchor.playing) {
      _playbackClockTicker?.stop();
      _playbackClockTickerBaseUs = null;
      return;
    }
    final tickerElapsedUs = elapsed.inMicroseconds;
    final baseUs = _playbackClockTickerBaseUs ??= tickerElapsedUs;
    final elapsedUs = tickerElapsedUs - baseUs;
    final nextPts = anchor.ptsUs + (elapsedUs * anchor.speed).round();
    _applyPlaybackClockState(
      ptsUs: nextPts,
      durationUs: anchor.durationUs,
      playing: true,
      speed: anchor.speed,
      fromNativeEvent: false,
    );
  }

  int _clampSeekTargetUs(int ptsUs) {
    final durationUs = timelineMetrics.effectiveDurationUs;
    if (durationUs <= 0) {
      return ptsUs < 0 ? 0 : ptsUs;
    }
    return ptsUs.clamp(0, durationUs).toInt();
  }

  double get timelineStartWidth =>
      trackDragHandleWidth + timelineControlsWidth() + trackDividerWidth;

  int get resolvedLoopStartUs =>
      loopStartUs().clamp(0, timelineMetrics.effectiveDurationUs).toInt();

  int get resolvedLoopEndUs {
    final durationUs = timelineMetrics.effectiveDurationUs;
    if (durationUs <= 0) return 0;
    final defaultEndUs = loopEndUs() <= 0 ? durationUs : loopEndUs();
    return defaultEndUs.clamp(resolvedLoopStartUs, durationUs).toInt();
  }

  List<int> get loopMarkerPtsUs {
    if (!loopRangeEnabled() || timelineMetrics.effectiveDurationUs <= 0) {
      return const [];
    }
    return [resolvedLoopStartUs, resolvedLoopEndUs];
  }

  bool get _currentPtsInsideLoopRange {
    if (!loopRangeEnabled()) return true;
    final startUs = resolvedLoopStartUs;
    final endUs = resolvedLoopEndUs;
    return currentPtsUs() >= startUs && currentPtsUs() < endUs;
  }

  void cancelLoopBoundaryTimer() {
    _loopBoundaryTimer?.cancel();
    _loopBoundaryTimer = null;
  }

  void scheduleLoopBoundaryTimer({int? fromPtsUs}) {
    cancelLoopBoundaryTimer();
    if (_disposed ||
        !loopRangeEnabled() ||
        nativeLoopRangeSynced() ||
        !isPlaying() ||
        playbackSpeed() <= 0 ||
        resolvedLoopEndUs <= resolvedLoopStartUs) {
      return;
    }

    final startUs = resolvedLoopStartUs;
    final endUs = resolvedLoopEndUs;
    final baseUs = (fromPtsUs ?? pendingSeekUs() ?? currentPtsUs())
        .clamp(startUs, endUs)
        .toInt();
    final remainingUs = endUs - baseUs;
    final delayUs = (remainingUs / playbackSpeed()).round();
    final delay = Duration(
      microseconds: delayUs
          .clamp(0, _maxLoopBoundaryDelay.inMicroseconds)
          .toInt(),
    );
    _loopBoundaryTimer = Timer(delay, _onLoopBoundaryTimer);
  }

  Future<void> _onLoopBoundaryTimer() async {
    _loopBoundaryTimer = null;
    if (_disposed) return;
    if (!loopRangeEnabled() ||
        nativeLoopRangeSynced() ||
        !isPlaying() ||
        resolvedLoopEndUs <= resolvedLoopStartUs) {
      return;
    }

    final startUs = resolvedLoopStartUs;
    final endUs = resolvedLoopEndUs;
    var pts = pendingSeekUs() ?? currentPtsUs();
    try {
      pts = await controller.currentPts();
    } catch (error, stack) {
      log.fine('loop boundary currentPts fallback failed', error, stack);
    }

    if (_disposed || !mounted() || !loopRangeEnabled() || !isPlaying()) return;
    if (pts < endUs - 12000) {
      scheduleLoopBoundaryTimer(fromPtsUs: pts);
      return;
    }
    seekTo(startUs);
  }

  Future<void> _pollState() async {
    if (_disposed || playerId() == null) return;
    final serial = ++_pollSerial;
    try {
      final snapshot = await _pollPlaybackSnapshot();
      if (_disposed || !mounted() || serial != _pollSerial) return;

      var pts = snapshot.currentPtsUs;
      final dur = snapshot.durationUs;
      final playing = snapshot.isPlaying;
      final seekUs = pendingSeekUs();
      var presentedFrameAnchors = _anchorsFromSnapshot(
        snapshot,
        allowFallbackAnchors: seekUs == null,
      );
      if (seekUs != null) {
        final seekAge = pendingSeekAt() == null
            ? Duration.zero
            : DateTime.now().difference(pendingSeekAt()!);
        const seekSettleToleranceUs = 50000;
        final clockSettled = (pts - seekUs).abs() <= seekSettleToleranceUs;
        final anchorsSettled = _presentedFrameAnchorsMatchSeekTarget(
          presentedFrameAnchors,
          seekUs,
          toleranceUs: seekSettleToleranceUs,
        );
        final settled = clockSettled && anchorsSettled;
        if (settled) {
          _clearPendingSeek();
        } else if (_needsPresentedFrameAnchors &&
            seekAge < _pendingSeekTimeout) {
          pts = seekUs;
          presentedFrameAnchors = const {};
        } else if (seekAge < _pendingSeekTimeout) {
          pts = seekUs;
        } else {
          _clearPendingSeek();
          if (!anchorsSettled) {
            presentedFrameAnchors = _fallbackPresentedFrameAnchorsForTimeline(
              seekUs,
            );
          }
        }
      }

      if (loopRangeEnabled() &&
          playing &&
          !nativeLoopRangeSynced() &&
          pendingSeekUs() == null &&
          resolvedLoopEndUs > resolvedLoopStartUs &&
          pts >= resolvedLoopEndUs) {
        seekTo(resolvedLoopStartUs);
        return;
      }

      if (pts == currentPtsUs() &&
          dur == durationUs() &&
          playing == isPlaying() &&
          (presentedFrameAnchors == null ||
              mapEquals(_state.presentedFrameAnchors, presentedFrameAnchors))) {
        return;
      }

      _setPlaybackClockAnchor(
        _PlaybackClockAnchor(
          ptsUs: pts,
          durationUs: dur,
          playing: playing,
          speed: playbackSpeed() <= 0 ? 1.0 : playbackSpeed(),
        ),
      );
      setPolledPlaybackState(
        pts,
        dur,
        playing,
        presentedFrameAnchors: presentedFrameAnchors,
      );
      if (playing) {
        scheduleLoopBoundaryTimer(fromPtsUs: pts);
      } else {
        cancelLoopBoundaryTimer();
      }
    } catch (error, stack) {
      log.fine('playback poll failed', error, stack);
    }
  }

  Future<PlaybackSnapshot> _pollPlaybackSnapshot() async {
    final includePresentedFrames = _needsPresentedFrameAnchors;
    try {
      return await controller.getPlaybackSnapshot(
        includePresentedFrames: includePresentedFrames,
      );
    } on MissingPluginException {
      return _pollLegacyPlaybackSnapshot(
        includePresentedFrames: includePresentedFrames,
      );
    }
  }

  Future<PlaybackSnapshot> _pollLegacyPlaybackSnapshot({
    required bool includePresentedFrames,
  }) async {
    final results = await Future.wait([
      controller.currentPts(),
      controller.duration(),
      controller.isPlaying(),
      if (includePresentedFrames) _pollPresentedFrameTimings(),
    ]);
    final presentedFrames = includePresentedFrames
        ? results[3] as Map<int, PresentedFrameTiming?>
        : const <int, PresentedFrameTiming?>{};
    return PlaybackSnapshot(
      currentPtsUs: results[0] as int,
      durationUs: results[1] as int,
      isPlaying: results[2] as bool,
      presentedFrames: {
        for (final entry in presentedFrames.entries)
          if (entry.value?.isValid ?? false) entry.key: entry.value!,
      },
    );
  }

  bool get _needsPresentedFrameAnchors =>
      _state.quickMarks.isNotEmpty ||
      _state.quickMarkDraft != null ||
      (_state.deckTab == MainWindowDeckTab.analysis && !_state.deckCollapsed);

  Map<int, QuickMarkAnchor>? _anchorsFromSnapshot(
    PlaybackSnapshot snapshot, {
    bool allowFallbackAnchors = true,
  }) {
    if (!_needsPresentedFrameAnchors) return null;
    if (trackManager.isEmpty) return const {};
    final anchors = <int, QuickMarkAnchor>{};
    for (final entry in trackManager.entries) {
      final anchor = _quickMarkAnchorFromPresentedFrame(
        fileId: entry.info.fileId,
        timing: snapshot.presentedFrames[entry.info.fileId],
        fallbackPtsUs: _trackPtsForTimelinePts(
          entry.info.fileId,
          snapshot.currentPtsUs,
        ),
        allowFallbackAnchor: allowFallbackAnchors,
      );
      if (anchor != null) {
        anchors[entry.info.fileId] = anchor;
      }
    }
    _traceQuickMarkAnchors(snapshot: snapshot, anchors: anchors);
    return anchors;
  }

  Future<Map<int, PresentedFrameTiming?>> _pollPresentedFrameTimings() async {
    if (trackManager.isEmpty) return const {};
    final entries = trackManager.entries.toList(growable: false);
    final timings = await Future.wait([
      for (final entry in entries)
        _safeCurrentPresentedFrame(entry.info.fileId),
    ]);
    return {
      for (var i = 0; i < entries.length; i++)
        entries[i].info.fileId: timings[i],
    };
  }

  Future<PresentedFrameTiming?> _safeCurrentPresentedFrame(int fileId) async {
    try {
      return await controller.currentPresentedFrame(fileId);
    } catch (error, stack) {
      log.fine(
        'current presented frame poll failed: fileId=$fileId',
        error,
        stack,
      );
      return null;
    }
  }

  bool _presentedFrameAnchorsMatchSeekTarget(
    Map<int, QuickMarkAnchor>? anchors,
    int targetPtsUs, {
    required int toleranceUs,
  }) {
    if (anchors == null || anchors.isEmpty) return false;
    for (final anchor in anchors.values) {
      final expectedPtsUs = _trackPtsForTimelinePts(anchor.fileId, targetPtsUs);
      if ((anchor.ptsUs - expectedPtsUs).abs() > toleranceUs) {
        return false;
      }
    }
    return true;
  }

  void _clearPendingSeek() {
    if (pendingSeekUs() != null) {
      _seekPreviewEventSerial++;
    }
    setPendingSeek(null, null);
  }

  Map<int, QuickMarkAnchor> _fallbackPresentedFrameAnchorsForTimeline(
    int timelinePtsUs, {
    int durationUs = 0,
  }) {
    if (trackManager.isEmpty) return const {};
    return {
      for (final entry in trackManager.entries)
        entry.info.fileId: QuickMarkAnchor(
          fileId: entry.info.fileId,
          ptsUs: _trackPtsForTimelinePts(entry.info.fileId, timelinePtsUs),
          dtsUs: _trackPtsForTimelinePts(entry.info.fileId, timelinePtsUs),
          durationUs: durationUs,
        ),
    };
  }

  int _trackPtsForTimelinePts(int fileId, int timelinePtsUs) {
    final offsetUs = _state.syncOffsets[fileId] ?? 0;
    return (timelinePtsUs - offsetUs).clamp(0, 1 << 62).toInt();
  }

  QuickMarkAnchor? _quickMarkAnchorFromPresentedFrame({
    required int fileId,
    required PresentedFrameTiming? timing,
    required int fallbackPtsUs,
    int? fallbackDtsUs,
    int fallbackDurationUs = 0,
    int? expectedPtsUs,
    bool requireExpectedPtsMatch = false,
    bool allowFallbackAnchor = true,
  }) {
    QuickMarkAnchor? fallback() => allowFallbackAnchor
        ? QuickMarkAnchor(
            fileId: fileId,
            ptsUs: fallbackPtsUs,
            dtsUs: fallbackDtsUs ?? fallbackPtsUs,
            durationUs: fallbackDurationUs,
          )
        : null;

    if (timing == null || !timing.isValid) {
      return fallback();
    }

    final timingPtsUs = timing.ptsUs;
    final expectedUs = expectedPtsUs ?? fallbackPtsUs;
    final hasExpectedPts = expectedUs >= 0 && timingPtsUs >= 0;
    final matchesExpected =
        !hasExpectedPts ||
        (timingPtsUs - expectedUs).abs() <= _presentedFrameAnchorToleranceUs;
    if (requireExpectedPtsMatch && !matchesExpected) {
      return fallback();
    }
    if (!timing.hasStableSourceIdentity && !matchesExpected) {
      return fallback();
    }

    final ptsUs = timingPtsUs >= 0 ? timingPtsUs : fallbackPtsUs;
    final dtsUs = timing.dtsUs == PresentedFrameTiming.noTimestampUs
        ? ptsUs
        : timing.dtsUs;
    final durationUs =
        timing.durationUs > 0 &&
            timing.durationUs <= _maxPresentedFrameDurationUs
        ? timing.durationUs
        : fallbackDurationUs;
    return QuickMarkAnchor(
      fileId: fileId,
      ptsUs: ptsUs,
      dtsUs: dtsUs,
      durationUs: durationUs,
      analysisFrameIndex: timing.analysisFrameIndex,
      frameIdentityMode: timing.frameIdentityMode,
      sourcePacketIndex: timing.sourcePacketIndex,
      sourcePacketSize: timing.sourcePacketSize,
      sourcePacketPos: timing.sourcePacketPos,
      sourcePacketPtsUs: timing.sourcePacketPtsUs,
      sourcePacketDtsUs: timing.sourcePacketDtsUs,
    );
  }

  void _traceQuickMarkAnchors({
    required PlaybackSnapshot snapshot,
    required Map<int, QuickMarkAnchor> anchors,
  }) {
    if (Platform.environment['VOIDPLAYER_QUICK_MARK_TRACE'] != '1') return;
    final rawFrames = snapshot.presentedFrames.entries
        .map(
          (entry) =>
              '${entry.key}:pts=${entry.value.ptsUs},dts=${entry.value.dtsUs},'
              'dur=${entry.value.durationUs},afi=${entry.value.analysisFrameIndex},'
              'spi=${entry.value.sourcePacketIndex},spp=${entry.value.sourcePacketPos}',
        )
        .join(';');
    final anchorText = anchors.entries
        .map((entry) => '${entry.key}:${_quickMarkAnchorTrace(entry.value)}')
        .join(';');
    final signature =
        'clock=${snapshot.currentPtsUs} playing=${snapshot.isPlaying} '
        'raw=[$rawFrames] anchors=[$anchorText]';
    if (signature == _lastQuickMarkAnchorTrace) return;
    _lastQuickMarkAnchorTrace = signature;
    log.finer('[QuickMarkTrace] playback snapshot $signature');
  }

  Future<void> setLoopRangeEnabled(bool enabled) async {
    if (_disposed) return;
    if (enabled) {
      _ensureLoopRangeInitialized();
      setLoopRangeEnabledState(true);
      _syncNativeLoopRange();
      await controller.pause();
      if (_disposed || !mounted()) return;
      cancelLoopBoundaryTimer();
      setPlaying(false);
      seekTo(resolvedLoopStartUs);
    } else {
      cancelLoopBoundaryTimer();
      setLoopRangeEnabledState(false);
      _syncNativeLoopRange();
    }
  }

  void applyStartupLoopRangeIfReady() {
    if (_disposed) return;
    if (startupLoopRangeApplied() || trackManager.isEmpty) return;
    final range = startupOptions.loopRange;
    if (range == null) return;
    final durationUs = timelineMetrics.effectiveDurationUs;
    if (durationUs <= 0) return;

    setStartupLoopRangeApplied(true);
    log.info('Applying startup loop range: ${range.startUs}:${range.endUs} us');
    setLoopRange(range.startUs, range.endUs);
    fireAndLog('apply startup loop range', setLoopRangeEnabled(true));
  }

  Future<void> setLoopRange(
    int startUs,
    int endUs, {
    bool seekToStart = false,
    bool seekOnlyIfStartChanged = false,
  }) async {
    if (_disposed) return;
    final previousStartUs = resolvedLoopStartUs;
    final clamped = _clampLoopRange(startUs, endUs);
    final clampedStartUs = clamped.startUs;
    final clampedEndUs = clamped.endUs;
    setLoopRangeState(clampedStartUs, clampedEndUs);
    if (loopRangeEnabled()) _syncNativeLoopRange();
    scheduleLoopBoundaryTimer();

    if (seekToStart &&
        loopRangeEnabled() &&
        (!seekOnlyIfStartChanged || clampedStartUs != previousStartUs)) {
      await controller.pause();
      if (_disposed || !mounted()) return;
      cancelLoopBoundaryTimer();
      setPlaying(false);
      seekTo(resolvedLoopStartUs);
    }
  }

  void previewLoopRange(int startUs, int endUs) {
    if (_disposed) return;
    final clamped = _clampLoopRange(startUs, endUs);
    final clampedStartUs = clamped.startUs;
    final clampedEndUs = clamped.endUs;
    if (clampedStartUs == resolvedLoopStartUs &&
        clampedEndUs == resolvedLoopEndUs) {
      return;
    }
    setLoopRangeState(clampedStartUs, clampedEndUs);
    if (loopRangeEnabled()) {
      setNativeLoopRangeSynced(false);
      scheduleLoopBoundaryTimer();
    }
  }

  Future<void> commitLoopRange({
    bool seekToStart = false,
    bool seekOnlyIfStartChanged = false,
  }) {
    return setLoopRange(
      resolvedLoopStartUs,
      resolvedLoopEndUs,
      seekToStart: seekToStart,
      seekOnlyIfStartChanged: seekOnlyIfStartChanged,
    );
  }

  void _syncNativeLoopRange() {
    if (_disposed) return;
    final enabled = loopRangeEnabled();
    final startUs = resolvedLoopStartUs;
    final endUs = resolvedLoopEndUs;
    final serial = ++_loopRangeSyncSerial;
    setNativeLoopRangeSynced(false);
    unawaited(
      controller
          .setLoopRange(enabled: enabled, startUs: startUs, endUs: endUs)
          .then((_) {
            if (_disposed || !mounted() || serial != _loopRangeSyncSerial) {
              return;
            }
            setNativeLoopRangeSynced(enabled);
            if (enabled) {
              cancelLoopBoundaryTimer();
            } else {
              scheduleLoopBoundaryTimer();
            }
          })
          .catchError((Object error, StackTrace stack) {
            log.warning(
              'native loop range sync failed: enabled=$enabled '
              'startUs=$startUs endUs=$endUs',
              error,
              stack,
            );
            if (_disposed || !mounted() || serial != _loopRangeSyncSerial) {
              return;
            }
            setNativeLoopRangeSynced(false);
            scheduleLoopBoundaryTimer();
          }),
    );
  }

  void _ensureLoopRangeInitialized() {
    final durationUs = timelineMetrics.effectiveDurationUs;
    if (durationUs <= 0) return;
    if (loopEndUs() <= loopStartUs() || loopEndUs() > durationUs) {
      final startUs = loopStartUs().clamp(0, durationUs).toInt();
      setLoopRangeState(startUs, durationUs);
    }
  }

  ({int startUs, int endUs}) _clampLoopRange(int startUs, int endUs) {
    final durationUs = timelineMetrics.effectiveDurationUs;
    final minRangeUs = durationUs > 10000 ? 10000 : 0;
    final maxStartUs = (durationUs - minRangeUs).clamp(0, durationUs);
    final clampedStartUs = startUs.clamp(0, maxStartUs).toInt();
    final clampedEndUs = endUs
        .clamp(clampedStartUs + minRangeUs, durationUs)
        .toInt();
    return (startUs: clampedStartUs, endUs: clampedEndUs);
  }

  void onSliderHover(int hoverUs, bool hovering) {
    if (_disposed) return;
    if (hoverPtsUs() == hoverUs && sliderHovering() == hovering) return;
    setSliderHoverState(hoverUs, hovering);
  }
}

class _PlaybackClockAnchor {
  final int ptsUs;
  final int durationUs;
  final bool playing;
  final double speed;

  const _PlaybackClockAnchor({
    required this.ptsUs,
    required this.durationUs,
    required this.playing,
    required this.speed,
  });
}
