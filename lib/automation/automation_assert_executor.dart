import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';
import 'dart:ui' as ui;

import '../actions/player_assert.dart';
import '../analysis/analysis_manager.dart';
import '../app_log.dart';
import '../platform/analysis_process_host.dart';
import '../windows/win32ffi.dart' deferred as win32;
import 'automation_probe.dart';
import 'automation_run_state.dart';

int _gpuBreakdownBytes(Map<String, dynamic> breakdown, String key) {
  return breakdown[key] as int? ?? 0;
}

class AutomationAssertExecutor {
  final AutomationProbe probe;
  final AutomationRunState state;
  final AnalysisProcessHost analysisProcesses;
  final int Function() effectiveDurationUs;

  const AutomationAssertExecutor({
    required this.probe,
    required this.state,
    required this.analysisProcesses,
    required this.effectiveDurationUs,
  });

  Future<void> execute(PlayerAssert assertion) async {
    final controller = probe.controller;
    switch (assertion) {
      case AssertPlaying():
        if (!await controller.isPlaying()) {
          throw AssertionError('Expected PLAYING, but isPlaying=false');
        }
      case AssertPaused():
        if (await controller.isPlaying()) {
          throw AssertionError('Expected PAUSED, but isPlaying=true');
        }
      case AssertPosition(:final ptsUs, :final toleranceMs):
        final actual = await controller.currentPts();
        final diff = (actual - ptsUs).abs();
        if (diff > toleranceMs * 1000) {
          throw AssertionError(
            'Expected position $ptsUs μs (±${toleranceMs}ms), got $actual μs (diff ${diff ~/ 1000}ms)',
          );
        }
      case AssertPositionRange(:final minUs, :final maxUs):
        final actual = await controller.currentPts();
        if (actual < minUs || actual > maxUs) {
          throw AssertionError(
            'Expected position in [$minUs, $maxUs] μs, got $actual μs',
          );
        }
      case AssertTrackCount(:final count):
        final tracks = await controller.getTracks();
        if (tracks.length != count) {
          throw AssertionError(
            'Expected track count $count, got ${tracks.length}',
          );
        }
      case AssertTrackOrder(:final fileIds):
        final layout = await controller.getLayout();
        final actual = layout.order.take(fileIds.length).toList();
        for (var i = 0; i < fileIds.length; i++) {
          if (actual[i] != fileIds[i]) {
            throw AssertionError(
              'Expected track order prefix $fileIds, got $actual',
            );
          }
        }
      case AssertNativeBackend(:final backend, :final available):
        final diagnostics = await controller.getDiagnostics();
        final actualBackend = diagnostics['backend'] as String? ?? '';
        final actualAvailable = diagnostics['available'] as bool? ?? false;
        if (actualBackend != backend || actualAvailable != available) {
          throw AssertionError(
            'Expected native backend=$backend available=$available, '
            'got backend=$actualBackend available=$actualAvailable',
          );
        }
      case AssertTrackMetadata(
        :final slot,
        :final formatName,
        :final decoderName,
      ):
        final tracks = await controller.getTracks();
        final matches = tracks.where((track) => track.slot == slot).toList();
        if (matches.isEmpty) {
          throw AssertionError('Expected track metadata for slot $slot');
        }
        final track = matches.first;
        if (track.formatName != formatName ||
            track.decoderName != decoderName) {
          throw AssertionError(
            'Expected track[$slot] format=$formatName decoder=$decoderName, '
            'got format=${track.formatName} decoder=${track.decoderName}',
          );
        }
      case AssertPresentedFrameRange(:final fileId, :final minUs, :final maxUs):
        final timing = await controller.currentPresentedFrame(fileId);
        final ptsUs = timing?.ptsUs;
        if (ptsUs == null || ptsUs < minUs || ptsUs > maxUs) {
          throw AssertionError(
            'Expected presented frame for fileId=$fileId in [$minUs, $maxUs] μs, got $ptsUs',
          );
        }
      case AssertNativeAudio(
        :final available,
        :final sampleRate,
        :final channels,
        :final activeTrack,
      ):
        final diagnostics = await controller.getDiagnostics();
        final actualAvailable = diagnostics['audioAvailable'] as bool? ?? false;
        final actualSampleRate = diagnostics['audioSampleRate'] as int? ?? 0;
        final actualChannels = diagnostics['audioChannels'] as int? ?? 0;
        final actualActiveTrack = diagnostics['activeAudioTrack'] as int? ?? -1;
        if (actualAvailable != available ||
            (sampleRate != null && actualSampleRate != sampleRate) ||
            (channels != null && actualChannels != channels) ||
            (activeTrack != null && actualActiveTrack != activeTrack)) {
          throw AssertionError(
            'Expected native audio available=$available sampleRate=$sampleRate '
            'channels=$channels activeTrack=$activeTrack, got '
            'available=$actualAvailable sampleRate=$actualSampleRate '
            'channels=$actualChannels activeTrack=$actualActiveTrack',
          );
        }
      case AssertDuration(:final ptsUs, :final toleranceMs):
        final actual = await controller.duration();
        final diff = (actual - ptsUs).abs();
        if (diff > toleranceMs * 1000) {
          throw AssertionError(
            'Expected duration $ptsUs μs (±${toleranceMs}ms), got $actual μs',
          );
        }
      case AssertEffectiveDuration(:final ptsUs, :final toleranceMs):
        final actual = effectiveDurationUs();
        final diff = (actual - ptsUs).abs();
        if (diff > toleranceMs * 1000) {
          throw AssertionError(
            'Expected effective duration $ptsUs μs (±${toleranceMs}ms), got $actual μs',
          );
        }
      case AssertLayoutMode(:final mode):
        final layout = await controller.getLayout();
        if (layout.mode != mode) {
          throw AssertionError(
            'Expected layout mode $mode, got ${layout.mode}',
          );
        }
      case AssertZoom(:final ratio, :final tolerance):
        final layout = await controller.getLayout();
        if ((layout.zoomRatio - ratio).abs() > tolerance) {
          throw AssertionError(
            'Expected zoom $ratio (±$tolerance), got ${layout.zoomRatio}',
          );
        }
      case AssertSplitPos(:final position, :final tolerance):
        final layout = await controller.getLayout();
        if ((layout.splitPos - position).abs() > tolerance) {
          throw AssertionError(
            'Expected split position $position (±$tolerance), got ${layout.splitPos}',
          );
        }
      case AssertViewOffset(:final x, :final y, :final tolerance):
        final layout = await controller.getLayout();
        final dx = (layout.viewOffsetX - x).abs();
        final dy = (layout.viewOffsetY - y).abs();
        if (dx > tolerance || dy > tolerance) {
          throw AssertionError(
            'Expected view offset ($x, $y) (±$tolerance), '
            'got (${layout.viewOffsetX}, ${layout.viewOffsetY})',
          );
        }
      case AssertViewCenterStable(:final baseline, :final tolerance):
        final expected = state.viewCenterBaselines[baseline];
        if (expected == null) {
          throw AssertionError(
            'Missing baseline for ASSERT_VIEW_CENTER_STABLE: $baseline',
          );
        }
        final actual = await probe.currentViewCenterMetric();
        final dx = (actual.x - expected.x).abs();
        final dy = (actual.y - expected.y).abs();
        if (dx > tolerance || dy > tolerance) {
          throw AssertionError(
            'Expected normalized view center to match $baseline '
            '(±$tolerance), got '
            '(${actual.x.toStringAsFixed(6)}, ${actual.y.toStringAsFixed(6)}) '
            'vs (${expected.x.toStringAsFixed(6)}, ${expected.y.toStringAsFixed(6)})',
          );
        }
      case AssertMainWindowBorderless():
        if (!Platform.isWindows) {
          throw AssertionError('ASSERT_MAIN_WINDOW_BORDERLESS is Windows-only');
        }
        await win32.loadLibrary();
        final hwnd = win32.Win32FFI.findCurrentMainWindow();
        if (hwnd == 0) {
          throw AssertionError('Expected main window HWND to exist');
        }
        if (win32.Win32FFI.hasOverlappedWindowFrame(hwnd)) {
          throw AssertionError(
            'Expected main window to be borderless in fullscreen, hwnd=$hwnd',
          );
        }
      case AssertCaptureEquals(:final expectedCapture, :final actualCapture):
        final expected = state.captures[expectedCapture];
        final actual = state.captures[actualCapture];
        if (expected == null || actual == null) {
          throw AssertionError(
            'Missing capture(s) for ASSERT_CAPTURE_EQUALS: $expectedCapture / $actualCapture',
          );
        }
        if (expected.hash != actual.hash) {
          throw AssertionError(
            'Expected capture $actualCapture to equal $expectedCapture, '
            'got ${actual.hash} != ${expected.hash}',
          );
        }
      case AssertCaptureChanged(:final beforeCapture, :final afterCapture):
        final before = state.captures[beforeCapture];
        final after = state.captures[afterCapture];
        if (before == null || after == null) {
          throw AssertionError(
            'Missing capture(s) for ASSERT_CAPTURE_CHANGED: $beforeCapture / $afterCapture',
          );
        }
        if (before.hash == after.hash) {
          throw AssertionError(
            'Expected capture $afterCapture to differ from $beforeCapture, '
            'but both hashes are ${before.hash}',
          );
        }
      case AssertCaptureHash(:final capture, :final hash):
        final actual = state.captures[capture];
        if (actual == null) {
          throw AssertionError(
            'Missing capture for ASSERT_CAPTURE_HASH: $capture',
          );
        }
        if (actual.hash != hash) {
          throw AssertionError(
            'Expected capture $capture hash=$hash, got ${actual.hash}',
          );
        }
      case AssertCaptureNotBlack(
        :final capture,
        :final minNonBlackRatio,
        :final minAvgLuma,
      ):
        final actual = state.captures[capture];
        if (actual == null) {
          throw AssertionError(
            'Missing capture for ASSERT_CAPTURE_NOT_BLACK: $capture',
          );
        }
        if (actual.nonBlackRatio < minNonBlackRatio ||
            actual.avgLuma < minAvgLuma) {
          throw AssertionError(
            'Expected capture $capture to be non-black '
            '(nonBlack>=${minNonBlackRatio.toStringAsFixed(4)}, avgLuma>=${minAvgLuma.toStringAsFixed(2)}), '
            'got nonBlack=${actual.nonBlackRatio.toStringAsFixed(4)}, '
            'avgLuma=${actual.avgLuma.toStringAsFixed(2)}, hash=${actual.hash}',
          );
        }
      case AssertCaptureHasDetail(:final capture, :final minLumaStdDev):
        final actual = state.captures[capture];
        if (actual == null) {
          throw AssertionError(
            'Missing capture for ASSERT_CAPTURE_HAS_DETAIL: $capture',
          );
        }
        final outputPath = actual.outputPath;
        if (outputPath == null || outputPath.isEmpty) {
          throw AssertionError(
            'ASSERT_CAPTURE_HAS_DETAIL requires CAPTURE_VIEWPORT with outputPath for $capture',
          );
        }
        final metric = await _measureCaptureDetail(outputPath);
        final summary = metric.summary(capture);
        log.info(summary);
        if (metric.lumaStdDev < minLumaStdDev) {
          throw AssertionError(
            '$summary below threshold (lumaStdDev>=$minLumaStdDev)',
          );
        }
      case AssertCaptureSplitDiff(
        :final capture,
        :final maxMeanAbsChannel,
        :final maxMeanAbsLuma,
        :final maxMaxChannel,
      ):
        final actual = state.captures[capture];
        if (actual == null) {
          throw AssertionError(
            'Missing capture for ASSERT_CAPTURE_SPLIT_DIFF: $capture',
          );
        }
        final outputPath = actual.outputPath;
        if (outputPath == null || outputPath.isEmpty) {
          throw AssertionError(
            'ASSERT_CAPTURE_SPLIT_DIFF requires CAPTURE_VIEWPORT with outputPath for $capture',
          );
        }
        final metric = await _measureCaptureSplitDiff(outputPath);
        final summary = metric.summary(capture);
        log.info(summary);
        if (metric.meanAbsChannel > maxMeanAbsChannel ||
            metric.meanAbsLuma > maxMeanAbsLuma ||
            metric.maxChannel > maxMaxChannel) {
          throw AssertionError(
            '$summary exceeds thresholds '
            '(meanAbsChannel<=$maxMeanAbsChannel, '
            'meanAbsLuma<=$maxMeanAbsLuma, maxChannel<=$maxMaxChannel)',
          );
        }
      case AssertCaptureDiff(
        :final expectedCapture,
        :final actualCapture,
        :final maxMeanAbsChannel,
        :final maxMeanAbsLuma,
        :final maxMaxChannel,
      ):
        final expected = state.captures[expectedCapture];
        final actual = state.captures[actualCapture];
        if (expected == null || actual == null) {
          throw AssertionError(
            'Missing capture(s) for ASSERT_CAPTURE_DIFF: $expectedCapture / $actualCapture',
          );
        }
        final expectedPath = expected.outputPath;
        final actualPath = actual.outputPath;
        if (expectedPath == null ||
            expectedPath.isEmpty ||
            actualPath == null ||
            actualPath.isEmpty) {
          throw AssertionError(
            'ASSERT_CAPTURE_DIFF requires CAPTURE_VIEWPORT outputPath for $expectedCapture and $actualCapture',
          );
        }
        final metric = await _measureCapturePairDiff(expectedPath, actualPath);
        final summary = metric.summary('$expectedCapture -> $actualCapture');
        log.info(summary);
        if (metric.meanAbsChannel > maxMeanAbsChannel ||
            metric.meanAbsLuma > maxMeanAbsLuma ||
            metric.maxChannel > maxMaxChannel) {
          throw AssertionError(
            '$summary exceeds thresholds '
            '(meanAbsChannel<=$maxMeanAbsChannel, '
            'meanAbsLuma<=$maxMeanAbsLuma, maxChannel<=$maxMaxChannel)',
          );
        }
      case AssertAnalysisProcessCount(:final count):
        final actual = analysisProcesses.analysisProcessCount;
        if (actual != count) {
          throw AssertionError(
            'Expected analysis process count $count, got $actual; '
            'exits=${analysisProcesses.analysisExitCodes}',
          );
        }
      case AssertAnalysisOverlay(
        :final active,
        :final type,
        :final opacity,
        :final opacityTolerance,
        :final trackCount,
      ):
        final manager = AnalysisManager.instance;
        final isActive = manager.overlayPanelVisible;
        if (isActive != active) {
          throw AssertionError(
            'Expected analysis overlay active=$active, got $isActive',
          );
        }
        if (trackCount != null &&
            manager.activeOverlayTrackFileIds.length != trackCount) {
          throw AssertionError(
            'Expected analysis overlay track count $trackCount, got '
            '${manager.activeOverlayTrackFileIds.length}',
          );
        }
        if (type != null && manager.overlayConfig.type != type) {
          throw AssertionError(
            'Expected analysis overlay type ${type.name}, '
            'got ${manager.overlayConfig.type.name}',
          );
        }
        if (opacity != null &&
            (manager.overlayConfig.opacity - opacity).abs() >
                opacityTolerance) {
          throw AssertionError(
            'Expected analysis overlay opacity $opacity '
            '(±$opacityTolerance), got ${manager.overlayConfig.opacity}',
          );
        }
      case AssertTrackBufferCountBelow(:final maxCount):
        final diagnostics = await controller.getDiagnostics();
        final tracks = diagnostics['tracks'] as List<dynamic>? ?? const [];
        for (final rawTrack in tracks) {
          final track = rawTrack as Map<dynamic, dynamic>;
          final bufferCount = track['bufferCount'] as int? ?? 0;
          final slot = track['slot'] as int? ?? -1;
          if (bufferCount > maxCount) {
            throw AssertionError(
              'Expected track[$slot] bufferCount <= $maxCount, got $bufferCount',
            );
          }
        }
      case AssertResourceUsageBelow(:final maxRssMb, :final maxDedicatedGpuMb):
        final actual = await probe.currentResourceUsageMetric();
        AutomationProbe.assertResourceMetricAvailable(
          actual,
          maxDedicatedGpuMb,
        );
        final rssMb = AutomationProbe.bytesToMb(actual.rssBytes);
        final privateMb = AutomationProbe.bytesToMb(actual.privateBytes);
        final gpuMb = AutomationProbe.bytesToMb(actual.dedicatedGpuBytes);
        if (rssMb > maxRssMb || gpuMb > maxDedicatedGpuMb) {
          throw AssertionError(
            'Expected resource usage <= rss=${maxRssMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${maxDedicatedGpuMb.toStringAsFixed(1)}MB; '
            'got rss=${rssMb.toStringAsFixed(1)}MB, '
            'private=${privateMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${gpuMb.toStringAsFixed(1)}MB',
          );
        }
      case AssertResourceUsageDeltaBelow(
        :final baseline,
        :final maxRssDeltaMb,
        :final maxDedicatedGpuDeltaMb,
        :final maxPrivateDeltaMb,
        :final maxKnownGpuDeltaMb,
      ):
        final expected = state.resourceBaselines[baseline];
        if (expected == null) {
          throw AssertionError(
            'Missing baseline for ASSERT_RESOURCE_USAGE_DELTA_BELOW: $baseline',
          );
        }
        final actual = await probe.currentResourceUsageMetric();
        AutomationProbe.assertResourceMetricAvailable(
          actual,
          maxDedicatedGpuDeltaMb,
        );
        final rssDeltaMb = AutomationProbe.bytesToMb(
          actual.rssBytes - expected.rssBytes,
        );
        final gpuDeltaMb = AutomationProbe.bytesToMb(
          actual.dedicatedGpuBytes - expected.dedicatedGpuBytes,
        );
        final privateDeltaMb = AutomationProbe.bytesToMb(
          actual.privateBytes - expected.privateBytes,
        );
        final heapAllocDeltaMb = AutomationProbe.bytesToMb(
          actual.heapAllocatedBytes - expected.heapAllocatedBytes,
        );
        final heapCommitDeltaMb = AutomationProbe.bytesToMb(
          actual.heapCommittedBytes - expected.heapCommittedBytes,
        );
        final heapReserveDeltaMb = AutomationProbe.bytesToMb(
          actual.heapReservedBytes - expected.heapReservedBytes,
        );
        final knownGpuDeltaMb = AutomationProbe.bytesToMb(
          _gpuBreakdownBytes(actual.gpuBreakdown, 'totalEstimatedBytes') -
              _gpuBreakdownBytes(expected.gpuBreakdown, 'totalEstimatedBytes'),
        );
        log.info(
          'ASSERT_RESOURCE_USAGE_DELTA_BELOW $baseline: '
          'rss=${rssDeltaMb.toStringAsFixed(1)}MB '
          'private=${privateDeltaMb.toStringAsFixed(1)}MB '
          'heapAlloc=${heapAllocDeltaMb.toStringAsFixed(1)}MB '
          'heapCommit=${heapCommitDeltaMb.toStringAsFixed(1)}MB '
          'heapReserve=${heapReserveDeltaMb.toStringAsFixed(1)}MB '
          'heaps=${actual.heapCount} '
          'dedicatedGpu=${gpuDeltaMb.toStringAsFixed(1)}MB '
          '${AutomationProbe.formatGpuBreakdown(actual.gpuBreakdown, dedicatedGpuBytes: actual.dedicatedGpuBytes)}',
        );
        final privateFailed =
            maxPrivateDeltaMb != null && privateDeltaMb > maxPrivateDeltaMb;
        final knownGpuFailed =
            maxKnownGpuDeltaMb != null && knownGpuDeltaMb > maxKnownGpuDeltaMb;
        if (rssDeltaMb > maxRssDeltaMb ||
            gpuDeltaMb > maxDedicatedGpuDeltaMb ||
            privateFailed ||
            knownGpuFailed) {
          throw AssertionError(
            'Expected resource delta from $baseline <= '
            'rss=${maxRssDeltaMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${maxDedicatedGpuDeltaMb.toStringAsFixed(1)}MB'
            '${maxPrivateDeltaMb == null ? '' : ', private=${maxPrivateDeltaMb.toStringAsFixed(1)}MB'}'
            '${maxKnownGpuDeltaMb == null ? '' : ', knownGpu=${maxKnownGpuDeltaMb.toStringAsFixed(1)}MB'}; '
            'got rss=${rssDeltaMb.toStringAsFixed(1)}MB, '
            'private=${privateDeltaMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${gpuDeltaMb.toStringAsFixed(1)}MB, '
            'knownGpu=${knownGpuDeltaMb.toStringAsFixed(1)}MB',
          );
        }
      case AssertNativeSeekCountDelta(:final baseline, :final expectedDelta):
        final expected = state.nativeSeekCountBaselines[baseline];
        if (expected == null) {
          throw AssertionError(
            'Missing baseline for ASSERT_NATIVE_SEEK_COUNT_DELTA: $baseline',
          );
        }
        final actual = probe.currentNativeSeekCount();
        final delta = actual - expected;
        if (delta != expectedDelta) {
          throw AssertionError(
            'Expected native seek count delta from $baseline to be '
            '$expectedDelta, got $delta (baseline=$expected, actual=$actual)',
          );
        }
    }
  }

  static Future<_CaptureSplitDiffMetric> _measureCaptureSplitDiff(
    String outputPath,
  ) async {
    final decoded = await _decodeCaptureRgba(outputPath);
    try {
      return _CaptureSplitDiffMetric.fromRgba(
        width: decoded.width,
        height: decoded.height,
        rgba: decoded.rgba,
      );
    } finally {
      decoded.dispose();
    }
  }

  static Future<_CaptureDetailMetric> _measureCaptureDetail(
    String outputPath,
  ) async {
    final decoded = await _decodeCaptureRgba(outputPath);
    try {
      return _CaptureDetailMetric.fromRgba(
        width: decoded.width,
        height: decoded.height,
        rgba: decoded.rgba,
      );
    } finally {
      decoded.dispose();
    }
  }

  static Future<_CapturePairDiffMetric> _measureCapturePairDiff(
    String expectedPath,
    String actualPath,
  ) async {
    final expected = await _decodeCaptureRgba(expectedPath);
    final actual = await _decodeCaptureRgba(actualPath);
    try {
      return _CapturePairDiffMetric.fromRgba(
        expectedWidth: expected.width,
        expectedHeight: expected.height,
        expectedRgba: expected.rgba,
        actualWidth: actual.width,
        actualHeight: actual.height,
        actualRgba: actual.rgba,
      );
    } finally {
      expected.dispose();
      actual.dispose();
    }
  }

  static Future<_DecodedCaptureRgba> _decodeCaptureRgba(
    String outputPath,
  ) async {
    final file = File(outputPath);
    if (!await file.exists()) {
      throw AssertionError('Capture file does not exist: $outputPath');
    }
    final bytes = await file.readAsBytes();
    final codec = await ui.instantiateImageCodec(bytes);
    final frame = await codec.getNextFrame();
    final image = frame.image;
    try {
      final rgbaData = await image.toByteData(
        format: ui.ImageByteFormat.rawRgba,
      );
      if (rgbaData == null) {
        throw AssertionError('Failed to decode capture pixels: $outputPath');
      }
      return _DecodedCaptureRgba(
        width: image.width,
        height: image.height,
        rgba: Uint8List.fromList(rgbaData.buffer.asUint8List()),
      );
    } finally {
      image.dispose();
      codec.dispose();
    }
  }
}

class _DecodedCaptureRgba {
  final int width;
  final int height;
  final Uint8List rgba;

  const _DecodedCaptureRgba({
    required this.width,
    required this.height,
    required this.rgba,
  });

  void dispose() {}
}

class _CaptureDetailMetric {
  final int width;
  final int height;
  final int samples;
  final double meanLuma;
  final double lumaStdDev;
  final double minLuma;
  final double maxLuma;

  const _CaptureDetailMetric({
    required this.width,
    required this.height,
    required this.samples,
    required this.meanLuma,
    required this.lumaStdDev,
    required this.minLuma,
    required this.maxLuma,
  });

  factory _CaptureDetailMetric.fromRgba({
    required int width,
    required int height,
    required Uint8List rgba,
  }) {
    final expectedBytes = width * height * 4;
    if (width <= 0 || height <= 0 || rgba.lengthInBytes < expectedBytes) {
      throw AssertionError(
        'Capture RGBA buffer is invalid: ${rgba.lengthInBytes} < $expectedBytes',
      );
    }

    var sum = 0.0;
    var sumSquares = 0.0;
    var minLuma = double.infinity;
    var maxLuma = double.negativeInfinity;
    final samples = width * height;
    for (var i = 0; i < expectedBytes; i += 4) {
      final luma =
          0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
      sum += luma;
      sumSquares += luma * luma;
      minLuma = math.min(minLuma, luma);
      maxLuma = math.max(maxLuma, luma);
    }

    final mean = sum / samples;
    final variance = math.max(0.0, sumSquares / samples - mean * mean);
    return _CaptureDetailMetric(
      width: width,
      height: height,
      samples: samples,
      meanLuma: mean,
      lumaStdDev: math.sqrt(variance),
      minLuma: minLuma,
      maxLuma: maxLuma,
    );
  }

  String summary(String capture) {
    return 'ASSERT_CAPTURE_HAS_DETAIL $capture: '
        'size=${width}x$height samples=$samples '
        'meanLuma=${meanLuma.toStringAsFixed(4)} '
        'lumaStdDev=${lumaStdDev.toStringAsFixed(4)} '
        'minLuma=${minLuma.toStringAsFixed(4)} '
        'maxLuma=${maxLuma.toStringAsFixed(4)}';
  }
}

class _CaptureSplitDiffMetric {
  final int width;
  final int height;
  final int samples;
  final double meanAbsChannel;
  final double rmseChannel;
  final int maxChannel;
  final double meanAbsLuma;
  final double meanSignedLumaRightMinusLeft;
  final double rmseLuma;
  final double maxAbsLuma;

  const _CaptureSplitDiffMetric({
    required this.width,
    required this.height,
    required this.samples,
    required this.meanAbsChannel,
    required this.rmseChannel,
    required this.maxChannel,
    required this.meanAbsLuma,
    required this.meanSignedLumaRightMinusLeft,
    required this.rmseLuma,
    required this.maxAbsLuma,
  });

  factory _CaptureSplitDiffMetric.fromRgba({
    required int width,
    required int height,
    required Uint8List rgba,
  }) {
    final halfWidth = width ~/ 2;
    const centerCrop = 4;
    const edgeCrop = 0;
    final compareWidth = halfWidth - centerCrop - edgeCrop;
    if (width < 4 || height <= 0 || compareWidth <= 0) {
      throw AssertionError(
        'Capture is too small for split diff: ${width}x$height',
      );
    }
    final expectedBytes = width * height * 4;
    if (rgba.lengthInBytes < expectedBytes) {
      throw AssertionError(
        'Capture RGBA buffer is too small: ${rgba.lengthInBytes} < $expectedBytes',
      );
    }

    var channelAbsSum = 0.0;
    var channelSqSum = 0.0;
    var maxChannel = 0;
    var lumaAbsSum = 0.0;
    var lumaSignedSum = 0.0;
    var lumaSqSum = 0.0;
    var maxAbsLuma = 0.0;
    var samples = 0;
    final rightStart = width - halfWidth;

    for (var y = 0; y < height; y++) {
      final row = y * width * 4;
      for (var x = edgeCrop; x < edgeCrop + compareWidth; x++) {
        final left = row + x * 4;
        final right = row + (rightStart + x) * 4;
        final dr = rgba[right] - rgba[left];
        final dg = rgba[right + 1] - rgba[left + 1];
        final db = rgba[right + 2] - rgba[left + 2];
        final adr = dr.abs();
        final adg = dg.abs();
        final adb = db.abs();
        channelAbsSum += adr + adg + adb;
        channelSqSum += dr * dr + dg * dg + db * db;
        maxChannel = math.max(maxChannel, math.max(adr, math.max(adg, adb)));

        final lumaDiff = 0.2126 * dr + 0.7152 * dg + 0.0722 * db;
        final absLuma = lumaDiff.abs();
        lumaAbsSum += absLuma;
        lumaSignedSum += lumaDiff;
        lumaSqSum += lumaDiff * lumaDiff;
        maxAbsLuma = math.max(maxAbsLuma, absLuma);
        samples++;
      }
    }

    return _CaptureSplitDiffMetric(
      width: width,
      height: height,
      samples: samples,
      meanAbsChannel: channelAbsSum / (samples * 3),
      rmseChannel: math.sqrt(channelSqSum / (samples * 3)),
      maxChannel: maxChannel,
      meanAbsLuma: lumaAbsSum / samples,
      meanSignedLumaRightMinusLeft: lumaSignedSum / samples,
      rmseLuma: math.sqrt(lumaSqSum / samples),
      maxAbsLuma: maxAbsLuma,
    );
  }

  String summary(String capture) {
    return 'ASSERT_CAPTURE_SPLIT_DIFF $capture: '
        'size=${width}x$height samples=$samples '
        'meanAbsChannel=${meanAbsChannel.toStringAsFixed(4)} '
        'rmseChannel=${rmseChannel.toStringAsFixed(4)} '
        'maxChannel=$maxChannel '
        'meanAbsLuma=${meanAbsLuma.toStringAsFixed(4)} '
        'meanSignedLumaRightMinusLeft=${meanSignedLumaRightMinusLeft.toStringAsFixed(4)} '
        'rmseLuma=${rmseLuma.toStringAsFixed(4)} '
        'maxAbsLuma=${maxAbsLuma.toStringAsFixed(4)}';
  }
}

class _CapturePairDiffMetric {
  final int width;
  final int height;
  final int samples;
  final double meanAbsChannel;
  final double rmseChannel;
  final int maxChannel;
  final double meanAbsLuma;
  final double meanSignedLumaActualMinusExpected;
  final double rmseLuma;
  final double maxAbsLuma;

  const _CapturePairDiffMetric({
    required this.width,
    required this.height,
    required this.samples,
    required this.meanAbsChannel,
    required this.rmseChannel,
    required this.maxChannel,
    required this.meanAbsLuma,
    required this.meanSignedLumaActualMinusExpected,
    required this.rmseLuma,
    required this.maxAbsLuma,
  });

  factory _CapturePairDiffMetric.fromRgba({
    required int expectedWidth,
    required int expectedHeight,
    required Uint8List expectedRgba,
    required int actualWidth,
    required int actualHeight,
    required Uint8List actualRgba,
  }) {
    if (expectedWidth != actualWidth || expectedHeight != actualHeight) {
      throw AssertionError(
        'Capture sizes differ: expected=${expectedWidth}x$expectedHeight, actual=${actualWidth}x$actualHeight',
      );
    }
    final width = expectedWidth;
    final height = expectedHeight;
    if (width <= 0 || height <= 0) {
      throw AssertionError('Capture is empty: ${width}x$height');
    }
    final expectedBytes = width * height * 4;
    if (expectedRgba.lengthInBytes < expectedBytes ||
        actualRgba.lengthInBytes < expectedBytes) {
      throw AssertionError('Capture RGBA buffer is too small');
    }

    var channelAbsSum = 0.0;
    var channelSqSum = 0.0;
    var maxChannel = 0;
    var lumaAbsSum = 0.0;
    var lumaSignedSum = 0.0;
    var lumaSqSum = 0.0;
    var maxAbsLuma = 0.0;
    final samples = width * height;

    for (var pixel = 0; pixel < samples; pixel++) {
      final off = pixel * 4;
      final dr = actualRgba[off] - expectedRgba[off];
      final dg = actualRgba[off + 1] - expectedRgba[off + 1];
      final db = actualRgba[off + 2] - expectedRgba[off + 2];
      final adr = dr.abs();
      final adg = dg.abs();
      final adb = db.abs();
      channelAbsSum += adr + adg + adb;
      channelSqSum += dr * dr + dg * dg + db * db;
      maxChannel = math.max(maxChannel, math.max(adr, math.max(adg, adb)));

      final lumaDiff = 0.2126 * dr + 0.7152 * dg + 0.0722 * db;
      final absLuma = lumaDiff.abs();
      lumaAbsSum += absLuma;
      lumaSignedSum += lumaDiff;
      lumaSqSum += lumaDiff * lumaDiff;
      maxAbsLuma = math.max(maxAbsLuma, absLuma);
    }

    return _CapturePairDiffMetric(
      width: width,
      height: height,
      samples: samples,
      meanAbsChannel: channelAbsSum / (samples * 3),
      rmseChannel: math.sqrt(channelSqSum / (samples * 3)),
      maxChannel: maxChannel,
      meanAbsLuma: lumaAbsSum / samples,
      meanSignedLumaActualMinusExpected: lumaSignedSum / samples,
      rmseLuma: math.sqrt(lumaSqSum / samples),
      maxAbsLuma: maxAbsLuma,
    );
  }

  String summary(String label) {
    return 'ASSERT_CAPTURE_DIFF $label: '
        'size=${width}x$height samples=$samples '
        'meanAbsChannel=${meanAbsChannel.toStringAsFixed(4)} '
        'rmseChannel=${rmseChannel.toStringAsFixed(4)} '
        'maxChannel=$maxChannel '
        'meanAbsLuma=${meanAbsLuma.toStringAsFixed(4)} '
        'meanSignedLumaActualMinusExpected=${meanSignedLumaActualMinusExpected.toStringAsFixed(4)} '
        'rmseLuma=${rmseLuma.toStringAsFixed(4)} '
        'maxAbsLuma=${maxAbsLuma.toStringAsFixed(4)}';
  }
}
