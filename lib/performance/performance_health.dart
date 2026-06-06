import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../feedback/app_feedback.dart';
import '../l10n/app_localizations.dart';
import '../native_player/native_player_api.dart';
import '../preferences/playback_preferences.dart';

enum PerformanceHealthLevel { ok, warning, severe }

enum PerformanceHealthKind {
  ok,
  nativeRenderPressure,
  externalDisplayPressure,
  decodePressure,
  playbackCadencePressure,
}

class PerformanceHealthSnapshot {
  final PerformanceHealthLevel level;
  final PerformanceHealthKind kind;
  final String reason;
  final double displayRefreshHz;
  final double displayTickHz;
  final double layoutDrawHz;
  final double layoutIntentHz;
  final double drawP95Us;
  final double backendP95Us;
  final double metalP95Us;
  final double hostIntervalP95Ms;
  final double playbackCadenceRatio;
  final double presentedFrameRateHz;
  final double expectedFrameRateHz;
  final int metalBufferExhaustionCount;
  final int metalBufferExhaustionDelta;
  final int metalFailureCount;
  final int metalFailureDelta;
  final int largeGapCount;
  final int largeGapDelta;
  final int monotonicViolationCount;
  final int presentedFramePtsDistinctCount;
  final int presentedFramePtsAdvanceUs;
  final int presentedFrameExpectedIntervalUs;
  final bool playing;
  final int trackCount;

  const PerformanceHealthSnapshot({
    required this.level,
    required this.kind,
    required this.reason,
    required this.displayRefreshHz,
    required this.displayTickHz,
    required this.layoutDrawHz,
    required this.layoutIntentHz,
    required this.drawP95Us,
    required this.backendP95Us,
    required this.metalP95Us,
    required this.hostIntervalP95Ms,
    required this.playbackCadenceRatio,
    required this.presentedFrameRateHz,
    required this.expectedFrameRateHz,
    required this.metalBufferExhaustionCount,
    required this.metalBufferExhaustionDelta,
    required this.metalFailureCount,
    required this.metalFailureDelta,
    required this.largeGapCount,
    required this.largeGapDelta,
    required this.monotonicViolationCount,
    required this.presentedFramePtsDistinctCount,
    required this.presentedFramePtsAdvanceUs,
    required this.presentedFrameExpectedIntervalUs,
    required this.playing,
    required this.trackCount,
  });

  factory PerformanceHealthSnapshot.ok({int trackCount = 0}) =>
      PerformanceHealthSnapshot(
        level: PerformanceHealthLevel.ok,
        kind: PerformanceHealthKind.ok,
        reason: '',
        displayRefreshHz: 0,
        displayTickHz: 0,
        layoutDrawHz: 0,
        layoutIntentHz: 0,
        drawP95Us: 0,
        backendP95Us: 0,
        metalP95Us: 0,
        hostIntervalP95Ms: 0,
        playbackCadenceRatio: 0,
        presentedFrameRateHz: 0,
        expectedFrameRateHz: 0,
        metalBufferExhaustionCount: 0,
        metalBufferExhaustionDelta: 0,
        metalFailureCount: 0,
        metalFailureDelta: 0,
        largeGapCount: 0,
        largeGapDelta: 0,
        monotonicViolationCount: 0,
        presentedFramePtsDistinctCount: 0,
        presentedFramePtsAdvanceUs: 0,
        presentedFrameExpectedIntervalUs: 0,
        playing: false,
        trackCount: trackCount,
      );

  factory PerformanceHealthSnapshot.fromDiagnostics(
    Map<String, dynamic> diagnostics, {
    PerformanceHealthSnapshot? previous,
  }) {
    final trackCount = _intValue(
      diagnostics['trackCount'] ?? diagnostics['nativeTrackDiagnosticCount'],
    );
    if (diagnostics.isEmpty || trackCount <= 0) {
      return PerformanceHealthSnapshot.ok(trackCount: trackCount);
    }
    final displayRefreshHz = _doubleValue(
      diagnostics['displayRefreshHzEstimate'],
    );
    final displayTickHz = _doubleValue(diagnostics['displayTickHz']);
    final layoutDrawHz = _doubleValue(diagnostics['layoutDrawHz']);
    final layoutIntentHz = _doubleValue(diagnostics['layoutIntentHz']);
    final drawP95Us = _doubleValue(diagnostics['nativeRendererDrawP95Us']);
    final backendP95Us = _doubleValue(
      diagnostics['nativeRendererDrawBackendP95Us'],
    );
    final metalP95Us = _doubleValue(diagnostics['metalCommandCompletionP95Us']);
    final hostIntervalP95Ms = _doubleValue(
      diagnostics['presentedFrameHostIntervalP95Ms'],
    );
    final metalBufferExhaustionCount = _intValue(
      diagnostics['metalBufferExhaustionCount'],
    );
    final metalFailureCount = _intValue(
      diagnostics['metalCommandFailureCount'],
    );
    final largeGapCount = _intValue(
      diagnostics['presentedFramePtsLargeGapCount'],
    );
    final monotonicViolationCount = _intValue(
      diagnostics['presentedFramePtsMonotonicViolationCount'],
    );
    final presentedFramePtsDistinctCount = _intValue(
      diagnostics['presentedFramePtsDistinctCount'],
    );
    final presentedFramePtsAdvanceUs = _intValue(
      diagnostics['presentedFramePtsAdvanceUs'],
    );
    final presentedFrameExpectedIntervalUs = _intValue(
      diagnostics['presentedFrameExpectedIntervalUs'] ??
          diagnostics['presentedFrameDurationLastUs'],
    );
    final playing = _boolValue(diagnostics['isPlaying']);
    final metalBufferDelta = previous == null
        ? 0
        : math.max(
            0,
            metalBufferExhaustionCount - previous.metalBufferExhaustionCount,
          );
    final metalFailureDelta = previous == null
        ? 0
        : math.max(0, metalFailureCount - previous.metalFailureCount);
    final largeGapDelta = previous == null
        ? 0
        : math.max(0, largeGapCount - previous.largeGapCount);
    final cadence = _playbackCadence(
      playing: playing,
      distinctCount: presentedFramePtsDistinctCount,
      advanceUs: presentedFramePtsAdvanceUs,
      expectedIntervalUs: presentedFrameExpectedIntervalUs,
      previous: previous,
    );

    final inputOrPlaybackActive = playing || layoutIntentHz >= 10;
    final renderLatencySlow =
        inputOrPlaybackActive &&
        (drawP95Us >= 11_000 || backendP95Us >= 10_000 || metalP95Us >= 11_000);
    final renderLatencySevere =
        inputOrPlaybackActive &&
        (drawP95Us >= 22_000 || backendP95Us >= 20_000 || metalP95Us >= 22_000);
    final nativeHardPressure = metalBufferDelta > 0 || metalFailureDelta > 0;
    final nativeSlow = renderLatencySlow || nativeHardPressure;
    final nativeSevere = renderLatencySevere || nativeHardPressure;
    final displayTarget = displayRefreshHz >= 50
        ? displayRefreshHz
        : _fallbackDisplayTargetHz;
    final displayTickLow =
        inputOrPlaybackActive &&
        displayTickHz > 0 &&
        displayTickHz < displayTarget * 0.72;
    final layoutDrawLow =
        layoutIntentHz >= 30 &&
        layoutDrawHz > 0 &&
        layoutDrawHz < math.min(layoutIntentHz, displayTarget) * 0.70;
    final expectedFrameIntervalMs = presentedFrameExpectedIntervalUs > 0
        ? presentedFrameExpectedIntervalUs / 1000.0
        : 1000.0 / displayTarget;
    final hostIntervalHigh =
        playing &&
        hostIntervalP95Ms > 0 &&
        hostIntervalP95Ms >
            math.max(1000.0 / displayTarget, expectedFrameIntervalMs) * 1.8;
    final externalPressure =
        !nativeSlow && (displayTickLow || layoutDrawLow || hostIntervalHigh);

    final decodePressure = playing && _hasDecodePressure(diagnostics);
    final presentationTimelineAnomaly =
        playing && (largeGapDelta > 0 || monotonicViolationCount > 0);
    final cadenceWarning =
        playing &&
        ((cadence.hasEnoughSignal &&
                cadence.ratio > 0 &&
                cadence.ratio < 0.82) ||
            presentationTimelineAnomaly ||
            hostIntervalHigh);
    final cadenceSevere =
        playing &&
        ((cadence.hasEnoughSignal &&
                cadence.ratio > 0 &&
                cadence.ratio < 0.58) ||
            monotonicViolationCount > 0);

    PerformanceHealthLevel level = PerformanceHealthLevel.ok;
    PerformanceHealthKind kind = PerformanceHealthKind.ok;
    var reason = '';
    if (nativeSlow) {
      level = nativeSevere
          ? PerformanceHealthLevel.severe
          : PerformanceHealthLevel.warning;
      kind = PerformanceHealthKind.nativeRenderPressure;
      reason = 'native-render';
    } else if (decodePressure) {
      level = PerformanceHealthLevel.warning;
      kind = PerformanceHealthKind.decodePressure;
      reason = 'decode-buffer';
    } else if (cadenceWarning) {
      level = cadenceSevere
          ? PerformanceHealthLevel.severe
          : PerformanceHealthLevel.warning;
      kind = PerformanceHealthKind.playbackCadencePressure;
      reason = 'playback-cadence';
    } else if (externalPressure) {
      level = PerformanceHealthLevel.warning;
      kind = PerformanceHealthKind.externalDisplayPressure;
      reason = 'display-pressure';
    }

    return PerformanceHealthSnapshot(
      level: level,
      kind: kind,
      reason: reason,
      displayRefreshHz: displayRefreshHz,
      displayTickHz: displayTickHz,
      layoutDrawHz: layoutDrawHz,
      layoutIntentHz: layoutIntentHz,
      drawP95Us: drawP95Us,
      backendP95Us: backendP95Us,
      metalP95Us: metalP95Us,
      hostIntervalP95Ms: hostIntervalP95Ms,
      playbackCadenceRatio: cadence.ratio,
      presentedFrameRateHz: cadence.presentedHz,
      expectedFrameRateHz: cadence.expectedHz,
      metalBufferExhaustionCount: metalBufferExhaustionCount,
      metalBufferExhaustionDelta: metalBufferDelta,
      metalFailureCount: metalFailureCount,
      metalFailureDelta: metalFailureDelta,
      largeGapCount: largeGapCount,
      largeGapDelta: largeGapDelta,
      monotonicViolationCount: monotonicViolationCount,
      presentedFramePtsDistinctCount: presentedFramePtsDistinctCount,
      presentedFramePtsAdvanceUs: presentedFramePtsAdvanceUs,
      presentedFrameExpectedIntervalUs: presentedFrameExpectedIntervalUs,
      playing: playing,
      trackCount: trackCount,
    );
  }

  bool get isPressure => level != PerformanceHealthLevel.ok;

  String localizedTitle(BuildContext context) {
    final zh = Localizations.localeOf(context).languageCode == 'zh';
    return switch (kind) {
      PerformanceHealthKind.ok => zh ? '播放健康' : 'Playback health',
      PerformanceHealthKind.nativeRenderPressure =>
        zh ? 'Native 渲染压力' : 'Native render pressure',
      PerformanceHealthKind.externalDisplayPressure =>
        zh ? '显示/GPU 压力' : 'Display/GPU pressure',
      PerformanceHealthKind.decodePressure => zh ? '解码缓冲压力' : 'Decode pressure',
      PerformanceHealthKind.playbackCadencePressure =>
        zh ? '播放帧率不足' : 'Playback cadence',
    };
  }

  String localizedFeedback(BuildContext context) {
    final zh = Localizations.localeOf(context).languageCode == 'zh';
    return switch (kind) {
      PerformanceHealthKind.nativeRenderPressure =>
        zh
            ? 'Native 渲染压力较高，拖拽或播放可能不流畅。可关闭 overlay 或减少轨道。'
            : 'Native rendering is under pressure. Disable overlays or reduce tracks for smoother playback.',
      PerformanceHealthKind.externalDisplayPressure =>
        zh
            ? '显示/GPU 压力较高，拖拽可能不流畅。可暂停外部显示、录屏、串流或其他图形高负载任务。'
            : 'Display/GPU pressure is high. Pause external displays, screen recording, streaming, or other graphics-heavy work for smoother dragging.',
      PerformanceHealthKind.decodePressure =>
        zh
            ? '解码缓冲压力较高，建议减少轨道或关闭高负载媒体。'
            : 'Decode buffers are under pressure. Reduce tracks or close heavy media.',
      PerformanceHealthKind.playbackCadencePressure =>
        zh
            ? '播放呈现帧率低于素材帧率，画面可能卡顿。可打开性能监视器查看帧率与解码缓冲。'
            : 'Presented playback rate is below the media frame rate. Open the performance monitor to inspect frame rate and decode buffers.',
      PerformanceHealthKind.ok => zh ? '播放状态正常。' : 'Playback looks healthy.',
    };
  }

  String localizedDetail(BuildContext context) {
    final parts = <String>[];
    if (displayRefreshHz > 0 || displayTickHz > 0) {
      final tickText = displayTickHz > 0
          ? displayTickHz.toStringAsFixed(0)
          : 'idle';
      final refreshText = displayRefreshHz > 0
          ? displayRefreshHz.toStringAsFixed(0)
          : '?';
      parts.add('display $tickText/${refreshText}Hz');
    }
    if (drawP95Us > 0) {
      parts.add('draw p95 ${_usText(drawP95Us)}');
    }
    if (metalP95Us > 0) {
      parts.add('metal p95 ${_usText(metalP95Us)}');
    }
    if (metalBufferExhaustionCount > 0) {
      parts.add('ring $metalBufferExhaustionCount');
    }
    if (playing && largeGapCount > 0) {
      parts.add('gap $largeGapCount');
    }
    return parts.isEmpty ? '-' : parts.join(' · ');
  }

  @override
  bool operator ==(Object other) =>
      other is PerformanceHealthSnapshot &&
      level == other.level &&
      kind == other.kind &&
      reason == other.reason &&
      displayRefreshHz == other.displayRefreshHz &&
      displayTickHz == other.displayTickHz &&
      layoutDrawHz == other.layoutDrawHz &&
      layoutIntentHz == other.layoutIntentHz &&
      drawP95Us == other.drawP95Us &&
      backendP95Us == other.backendP95Us &&
      metalP95Us == other.metalP95Us &&
      hostIntervalP95Ms == other.hostIntervalP95Ms &&
      playbackCadenceRatio == other.playbackCadenceRatio &&
      presentedFrameRateHz == other.presentedFrameRateHz &&
      expectedFrameRateHz == other.expectedFrameRateHz &&
      metalBufferExhaustionCount == other.metalBufferExhaustionCount &&
      metalBufferExhaustionDelta == other.metalBufferExhaustionDelta &&
      metalFailureCount == other.metalFailureCount &&
      metalFailureDelta == other.metalFailureDelta &&
      largeGapCount == other.largeGapCount &&
      largeGapDelta == other.largeGapDelta &&
      monotonicViolationCount == other.monotonicViolationCount &&
      presentedFramePtsDistinctCount == other.presentedFramePtsDistinctCount &&
      presentedFramePtsAdvanceUs == other.presentedFramePtsAdvanceUs &&
      presentedFrameExpectedIntervalUs ==
          other.presentedFrameExpectedIntervalUs &&
      playing == other.playing &&
      trackCount == other.trackCount;

  @override
  int get hashCode => Object.hashAll([
    level,
    kind,
    reason,
    displayRefreshHz.round(),
    displayTickHz.round(),
    layoutDrawHz.round(),
    layoutIntentHz.round(),
    drawP95Us.round(),
    backendP95Us.round(),
    metalP95Us.round(),
    (hostIntervalP95Ms * 10).round(),
    (playbackCadenceRatio * 1000).round(),
    (presentedFrameRateHz * 10).round(),
    (expectedFrameRateHz * 10).round(),
    metalBufferExhaustionCount,
    metalBufferExhaustionDelta,
    metalFailureCount,
    metalFailureDelta,
    largeGapCount,
    largeGapDelta,
    monotonicViolationCount,
    presentedFramePtsDistinctCount,
    presentedFramePtsAdvanceUs,
    presentedFrameExpectedIntervalUs,
    playing,
    trackCount,
  ]);

  static const _fallbackDisplayTargetHz = 60.0;

  static bool _hasDecodePressure(Map<String, dynamic> diagnostics) {
    final tracks =
        diagnostics['nativeTrackDiagnostics'] ?? diagnostics['tracks'];
    if (tracks is! List) return false;
    for (final track in tracks) {
      if (track is! Map) continue;
      final bufferState = _intValue(track['bufferState']);
      final bufferCount = _intValue(track['bufferCount']);
      final bufferCapacity = _intValue(track['bufferCapacity']);
      if (bufferState == 1) return true;
      if (bufferCapacity > 0 && bufferCount <= 0) return true;
    }
    return false;
  }

  static _PlaybackCadenceSample _playbackCadence({
    required bool playing,
    required int distinctCount,
    required int advanceUs,
    required int expectedIntervalUs,
    required PerformanceHealthSnapshot? previous,
  }) {
    if (!playing || expectedIntervalUs <= 0 || advanceUs <= 0) {
      return const _PlaybackCadenceSample.zero();
    }
    final expectedHz = 1000000.0 / expectedIntervalUs;
    var measuredDistinct = math.max(0, distinctCount - 1);
    var measuredAdvanceUs = advanceUs;
    if (previous != null &&
        previous.playing &&
        previous.presentedFrameExpectedIntervalUs > 0 &&
        advanceUs >= previous.presentedFramePtsAdvanceUs &&
        distinctCount >= previous.presentedFramePtsDistinctCount) {
      measuredDistinct =
          distinctCount - previous.presentedFramePtsDistinctCount;
      measuredAdvanceUs = advanceUs - previous.presentedFramePtsAdvanceUs;
    }
    if (measuredAdvanceUs <= 0 || measuredDistinct <= 0) {
      return _PlaybackCadenceSample(
        ratio: 0,
        presentedHz: 0,
        expectedHz: expectedHz,
        hasEnoughSignal: false,
      );
    }
    final expectedFrames = measuredAdvanceUs / expectedIntervalUs;
    final presentedHz = measuredDistinct * 1000000.0 / measuredAdvanceUs;
    return _PlaybackCadenceSample(
      ratio: expectedFrames > 0 ? measuredDistinct / expectedFrames : 0,
      presentedHz: presentedHz,
      expectedHz: expectedHz,
      hasEnoughSignal: measuredAdvanceUs >= 500000 && expectedFrames >= 10,
    );
  }

  static int _intValue(Object? value) {
    if (value is int) return value;
    if (value is double) return value.toInt();
    if (value is num) return value.toInt();
    return 0;
  }

  static double _doubleValue(Object? value) {
    if (value is double) return value;
    if (value is num) return value.toDouble();
    return 0.0;
  }

  static bool _boolValue(Object? value) {
    if (value is bool) return value;
    if (value is num) return value != 0;
    return false;
  }

  static String _usText(double value) {
    if (value >= 1000) return '${(value / 1000.0).toStringAsFixed(1)}ms';
    return '${value.toStringAsFixed(0)}us';
  }
}

class _PlaybackCadenceSample {
  final double ratio;
  final double presentedHz;
  final double expectedHz;
  final bool hasEnoughSignal;

  const _PlaybackCadenceSample({
    required this.ratio,
    required this.presentedHz,
    required this.expectedHz,
    required this.hasEnoughSignal,
  });

  const _PlaybackCadenceSample.zero()
    : ratio = 0,
      presentedHz = 0,
      expectedHz = 0,
      hasEnoughSignal = false;
}

class PerformanceHealthFeedbackMonitor extends StatefulWidget {
  final bool enabled;
  final int trackCount;
  final bool profilerVisible;
  final PerformanceAlertPolicy alertPolicy;
  final VoidCallback onOpenProfiler;
  final NativePlayerApi api;

  const PerformanceHealthFeedbackMonitor({
    super.key,
    required this.enabled,
    required this.trackCount,
    required this.profilerVisible,
    this.alertPolicy = PerformanceAlertPolicy.sustained,
    required this.onOpenProfiler,
    this.api = const MethodChannelNativePlayerApi(),
  });

  @override
  State<PerformanceHealthFeedbackMonitor> createState() =>
      _PerformanceHealthFeedbackMonitorState();
}

@visibleForTesting
class PerformanceHealthFeedbackPolicy {
  static const pollInterval = Duration(milliseconds: 1500);
  static const cooldown = Duration(seconds: 45);
  static const requiredConsecutivePressureSamples = 3;

  int _pressureSamples = 0;
  DateTime? _lastFeedbackAt;

  int get pressureSamples => _pressureSamples;

  void reset() {
    _pressureSamples = 0;
    _lastFeedbackAt = null;
  }

  bool shouldShow({
    required PerformanceHealthSnapshot snapshot,
    required bool profilerVisible,
    required PerformanceAlertPolicy alertPolicy,
    required DateTime now,
  }) {
    if (alertPolicy == PerformanceAlertPolicy.disabled) {
      _pressureSamples = 0;
      return false;
    }
    if (!snapshot.isPressure) {
      _pressureSamples = math.max(0, _pressureSamples - 1);
      return false;
    }
    _pressureSamples += 1;
    if (_pressureSamples < requiredConsecutivePressureSamples) {
      return false;
    }
    if (profilerVisible) {
      return false;
    }
    final last = _lastFeedbackAt;
    if (last != null) {
      if (alertPolicy == PerformanceAlertPolicy.once) {
        return false;
      }
      if (now.difference(last) < cooldown) {
        return false;
      }
    }
    _lastFeedbackAt = now;
    return true;
  }
}

class _PerformanceHealthFeedbackMonitorState
    extends State<PerformanceHealthFeedbackMonitor> {
  Timer? _timer;
  Future<void>? _pollInFlight;
  PerformanceHealthSnapshot? _previous;
  final PerformanceHealthFeedbackPolicy _feedbackPolicy =
      PerformanceHealthFeedbackPolicy();

  @override
  void initState() {
    super.initState();
    _syncTimer();
  }

  @override
  void didUpdateWidget(covariant PerformanceHealthFeedbackMonitor oldWidget) {
    super.didUpdateWidget(oldWidget);
    _syncTimer();
    if (!widget.enabled || widget.trackCount <= 0) {
      _previous = null;
      _feedbackPolicy.reset();
    } else if (oldWidget.alertPolicy != widget.alertPolicy) {
      _feedbackPolicy.reset();
    }
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => const SizedBox.shrink();

  void _syncTimer() {
    final shouldRun = widget.enabled && widget.trackCount > 0;
    if (!shouldRun) {
      _timer?.cancel();
      _timer = null;
      return;
    }
    if (_timer != null) return;
    _timer = Timer.periodic(
      PerformanceHealthFeedbackPolicy.pollInterval,
      (_) => unawaited(_poll()),
    );
    unawaited(_poll());
  }

  Future<void> _poll() {
    final existing = _pollInFlight;
    if (existing != null) return existing;
    final next = _pollImpl().whenComplete(() => _pollInFlight = null);
    _pollInFlight = next;
    return next;
  }

  Future<void> _pollImpl() async {
    if (!mounted || !widget.enabled || widget.trackCount <= 0) return;
    Map<String, dynamic> diagnostics;
    try {
      diagnostics = await widget.api.getDiagnostics();
    } catch (_) {
      return;
    }
    if (!mounted) return;
    final snapshot = PerformanceHealthSnapshot.fromDiagnostics(
      diagnostics,
      previous: _previous,
    );
    _previous = snapshot;
    final shouldShow = _feedbackPolicy.shouldShow(
      snapshot: snapshot,
      profilerVisible: widget.profilerVisible,
      alertPolicy: widget.alertPolicy,
      now: DateTime.now(),
    );
    if (!shouldShow) return;
    final l = AppLocalizations.of(context);
    AppFeedbackScope.read(context).show(
      AppFeedbackMessage(
        text: snapshot.localizedFeedback(context),
        severity: snapshot.level == PerformanceHealthLevel.severe
            ? AppFeedbackSeverity.error
            : AppFeedbackSeverity.warning,
        actionLabel: l?.performanceMonitor ?? 'Performance',
        onAction: widget.onOpenProfiler,
        duration: const Duration(seconds: 6),
      ),
    );
  }
}
