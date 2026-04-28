import 'dart:async';

import '../app_log.dart';
import '../startup_options.dart';
import '../track_manager.dart';
import '../video_renderer_controller.dart';

class MainWindowPlaybackCoordinator {
  static const double trackDragHandleWidth = 28.0;
  static const double trackDividerWidth = 1.0;

  final VideoRendererController controller;
  final TrackManager trackManager;
  final StartupOptions startupOptions;
  final bool Function() mounted;
  final int? Function() textureId;
  final int Function() effectiveDurationUs;
  final double Function() timelineControlsWidth;
  final bool Function() isPlaying;
  final void Function(bool playing) setPlaying;
  final double Function() playbackSpeed;
  final void Function(double speed) setPlaybackSpeed;
  final int Function() currentPtsUs;
  final int Function() durationUs;
  final int? Function() pendingSeekUs;
  final DateTime? Function() pendingSeekAt;
  final void Function(int ptsUs) setSeekPreview;
  final void Function(int? ptsUs, DateTime? at) setPendingSeek;
  final void Function(int ptsUs, int durationUs, bool playing)
  setPolledPlaybackState;
  final bool Function() loopRangeEnabled;
  final void Function(bool enabled) setLoopRangeEnabledState;
  final bool Function() nativeLoopRangeSynced;
  final void Function(bool synced) setNativeLoopRangeSynced;
  final bool Function() startupLoopRangeApplied;
  final void Function(bool applied) setStartupLoopRangeApplied;
  final int Function() loopStartUs;
  final int Function() loopEndUs;
  final void Function(int startUs, int endUs) setLoopRangeState;
  final int Function() hoverPtsUs;
  final bool Function() sliderHovering;
  final void Function(int hoverUs, bool hovering) setSliderHoverState;

  Timer? _pollTimer;
  Timer? _loopBoundaryTimer;
  int _loopRangeSyncSerial = 0;

  MainWindowPlaybackCoordinator({
    required this.controller,
    required this.trackManager,
    required this.startupOptions,
    required this.mounted,
    required this.textureId,
    required this.effectiveDurationUs,
    required this.timelineControlsWidth,
    required this.isPlaying,
    required this.setPlaying,
    required this.playbackSpeed,
    required this.setPlaybackSpeed,
    required this.currentPtsUs,
    required this.durationUs,
    required this.pendingSeekUs,
    required this.pendingSeekAt,
    required this.setSeekPreview,
    required this.setPendingSeek,
    required this.setPolledPlaybackState,
    required this.loopRangeEnabled,
    required this.setLoopRangeEnabledState,
    required this.nativeLoopRangeSynced,
    required this.setNativeLoopRangeSynced,
    required this.startupLoopRangeApplied,
    required this.setStartupLoopRangeApplied,
    required this.loopStartUs,
    required this.loopEndUs,
    required this.setLoopRangeState,
    required this.hoverPtsUs,
    required this.sliderHovering,
    required this.setSliderHoverState,
  });

  void dispose() {
    _pollTimer?.cancel();
    _loopBoundaryTimer?.cancel();
  }

  void startPolling() {
    _pollTimer = Timer.periodic(
      const Duration(milliseconds: 200),
      (_) => _pollState(),
    );
  }

  void invalidateLoopRangeSync() {
    _loopRangeSyncSerial++;
  }

  void togglePlayPause() {
    if (isPlaying()) {
      unawaited(pause());
    } else {
      unawaited(play());
    }
  }

  Future<void> play() async {
    if (loopRangeEnabled() && !_currentPtsInsideLoopRange) {
      await _seekToAsync(resolvedLoopStartUs);
      if (!mounted()) return;
    }
    await controller.play();
    if (!mounted()) return;
    setPlaying(true);
    scheduleLoopBoundaryTimer();
  }

  Future<void> pause() async {
    await controller.pause();
    if (!mounted()) return;
    cancelLoopBoundaryTimer();
    setPlaying(false);
  }

  void setSpeed(double speed) {
    setPlaybackSpeed(speed > 0 ? speed : 1.0);
    unawaited(controller.setSpeed(speed));
    scheduleLoopBoundaryTimer();
  }

  void seekTo(int ptsUs) {
    unawaited(_seekToAsync(ptsUs).catchError((_) {}));
  }

  Future<void> _seekToAsync(int ptsUs) async {
    setSeekPreview(ptsUs);
    await controller.seek(ptsUs);
    if (!mounted()) return;
    scheduleLoopBoundaryTimer(fromPtsUs: ptsUs);
  }

  double get timelineStartWidth =>
      trackDragHandleWidth + timelineControlsWidth() + trackDividerWidth;

  int get resolvedLoopStartUs =>
      loopStartUs().clamp(0, effectiveDurationUs()).toInt();

  int get resolvedLoopEndUs {
    final durationUs = effectiveDurationUs();
    if (durationUs <= 0) return 0;
    final defaultEndUs = loopEndUs() <= 0 ? durationUs : loopEndUs();
    return defaultEndUs.clamp(resolvedLoopStartUs, durationUs).toInt();
  }

  List<int> get loopMarkerPtsUs {
    if (!loopRangeEnabled() || effectiveDurationUs() <= 0) return const [];
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
    if (!loopRangeEnabled() ||
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
    final delay = Duration(microseconds: delayUs.clamp(0, 1 << 31).toInt());
    _loopBoundaryTimer = Timer(delay, _onLoopBoundaryTimer);
  }

  Future<void> _onLoopBoundaryTimer() async {
    _loopBoundaryTimer = null;
    if (!loopRangeEnabled() ||
        !isPlaying() ||
        resolvedLoopEndUs <= resolvedLoopStartUs) {
      return;
    }

    final startUs = resolvedLoopStartUs;
    final endUs = resolvedLoopEndUs;
    var pts = pendingSeekUs() ?? currentPtsUs();
    try {
      pts = await controller.currentPts();
    } catch (_) {}

    if (!mounted() || !loopRangeEnabled() || !isPlaying()) return;
    if (pts < endUs - 12000) {
      scheduleLoopBoundaryTimer(fromPtsUs: pts);
      return;
    }
    seekTo(startUs);
  }

  Future<void> _pollState() async {
    if (textureId() == null) return;
    try {
      final results = await Future.wait([
        controller.currentPts(),
        controller.duration(),
        controller.isPlaying(),
      ]);
      if (!mounted()) return;

      var pts = results[0] as int;
      final dur = results[1] as int;
      final playing = results[2] as bool;
      final seekUs = pendingSeekUs();
      if (seekUs != null) {
        final seekAge = pendingSeekAt() == null
            ? Duration.zero
            : DateTime.now().difference(pendingSeekAt()!);
        final settled = (pts - seekUs).abs() <= 50000;
        if (settled) {
          setPendingSeek(null, null);
        } else if (seekAge < const Duration(milliseconds: 1500)) {
          pts = seekUs;
        } else {
          setPendingSeek(null, null);
        }
      }

      if (loopRangeEnabled() &&
          playing &&
          pendingSeekUs() == null &&
          resolvedLoopEndUs > resolvedLoopStartUs &&
          pts >= resolvedLoopEndUs) {
        seekTo(resolvedLoopStartUs);
        return;
      }

      if (pts == currentPtsUs() &&
          dur == durationUs() &&
          playing == isPlaying()) {
        return;
      }

      setPolledPlaybackState(pts, dur, playing);
      if (playing) {
        scheduleLoopBoundaryTimer(fromPtsUs: pts);
      } else {
        cancelLoopBoundaryTimer();
      }
    } catch (_) {}
  }

  Future<void> setLoopRangeEnabled(bool enabled) async {
    if (enabled) {
      _ensureLoopRangeInitialized();
      setLoopRangeEnabledState(true);
      _syncNativeLoopRange();
      await controller.pause();
      if (!mounted()) return;
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
    if (startupLoopRangeApplied() || trackManager.isEmpty) return;
    final range = startupOptions.loopRange;
    if (range == null) return;
    final durationUs = effectiveDurationUs();
    if (durationUs <= 0) return;

    setStartupLoopRangeApplied(true);
    log.info('Applying startup loop range: ${range.startUs}:${range.endUs} us');
    setLoopRange(range.startUs, range.endUs);
    unawaited(setLoopRangeEnabled(true));
  }

  Future<void> setLoopRange(
    int startUs,
    int endUs, {
    bool seekToStart = false,
    bool seekOnlyIfStartChanged = false,
  }) async {
    final durationUs = effectiveDurationUs();
    final previousStartUs = resolvedLoopStartUs;
    final minRangeUs = durationUs > 10000 ? 10000 : 0;
    final maxStartUs = (durationUs - minRangeUs).clamp(0, durationUs);
    final clampedStartUs = startUs.clamp(0, maxStartUs).toInt();
    final clampedEndUs = endUs
        .clamp(clampedStartUs + minRangeUs, durationUs)
        .toInt();

    setLoopRangeState(clampedStartUs, clampedEndUs);
    if (loopRangeEnabled()) _syncNativeLoopRange();
    scheduleLoopBoundaryTimer();

    if (seekToStart &&
        loopRangeEnabled() &&
        (!seekOnlyIfStartChanged || clampedStartUs != previousStartUs)) {
      await controller.pause();
      if (!mounted()) return;
      cancelLoopBoundaryTimer();
      setPlaying(false);
      seekTo(resolvedLoopStartUs);
    }
  }

  void _syncNativeLoopRange() {
    final enabled = loopRangeEnabled();
    final startUs = resolvedLoopStartUs;
    final endUs = resolvedLoopEndUs;
    final serial = ++_loopRangeSyncSerial;
    setNativeLoopRangeSynced(false);
    unawaited(
      controller
          .setLoopRange(enabled: enabled, startUs: startUs, endUs: endUs)
          .then((_) {
            if (!mounted() || serial != _loopRangeSyncSerial) return;
            setNativeLoopRangeSynced(enabled);
            if (enabled) {
              cancelLoopBoundaryTimer();
            } else {
              scheduleLoopBoundaryTimer();
            }
          })
          .catchError((_) {
            if (!mounted() || serial != _loopRangeSyncSerial) return;
            setNativeLoopRangeSynced(false);
            scheduleLoopBoundaryTimer();
          }),
    );
  }

  void _ensureLoopRangeInitialized() {
    final durationUs = effectiveDurationUs();
    if (durationUs <= 0) return;
    if (loopEndUs() <= loopStartUs() || loopEndUs() > durationUs) {
      final startUs = loopStartUs().clamp(0, durationUs).toInt();
      setLoopRangeState(startUs, durationUs);
    }
  }

  void onSliderHover(int hoverUs, bool hovering) {
    if (hoverPtsUs() == hoverUs && sliderHovering() == hovering) return;
    setSliderHoverState(hoverUs, hovering);
  }
}
