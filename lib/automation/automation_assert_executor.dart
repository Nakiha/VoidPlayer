import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';
import 'dart:ui' as ui;

import '../actions/player_assert.dart';
import '../app_log.dart';
import '../windows/win32ffi.dart';
import '../windows/window_manager.dart';
import 'automation_probe.dart';
import 'automation_run_state.dart';

class AutomationAssertExecutor {
  final AutomationProbe probe;
  final AutomationRunState state;
  final AnalysisProcessManager analysisProcesses;

  const AutomationAssertExecutor({
    required this.probe,
    required this.state,
    required this.analysisProcesses,
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
      case AssertDuration(:final ptsUs, :final toleranceMs):
        final actual = await controller.duration();
        final diff = (actual - ptsUs).abs();
        if (diff > toleranceMs * 1000) {
          throw AssertionError(
            'Expected duration $ptsUs μs (±${toleranceMs}ms), got $actual μs',
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
        final hwnd = Win32FFI.findCurrentMainWindow();
        if (hwnd == 0) {
          throw AssertionError('Expected main window HWND to exist');
        }
        if (Win32FFI.hasOverlappedWindowFrame(hwnd)) {
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
        final gpuMb = AutomationProbe.bytesToMb(actual.dedicatedGpuBytes);
        if (rssMb > maxRssMb || gpuMb > maxDedicatedGpuMb) {
          throw AssertionError(
            'Expected resource usage <= rss=${maxRssMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${maxDedicatedGpuMb.toStringAsFixed(1)}MB; '
            'got rss=${rssMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${gpuMb.toStringAsFixed(1)}MB',
          );
        }
      case AssertResourceUsageDeltaBelow(
        :final baseline,
        :final maxRssDeltaMb,
        :final maxDedicatedGpuDeltaMb,
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
        if (rssDeltaMb > maxRssDeltaMb || gpuDeltaMb > maxDedicatedGpuDeltaMb) {
          throw AssertionError(
            'Expected resource delta from $baseline <= '
            'rss=${maxRssDeltaMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${maxDedicatedGpuDeltaMb.toStringAsFixed(1)}MB; '
            'got rss=${rssDeltaMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${gpuDeltaMb.toStringAsFixed(1)}MB',
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
