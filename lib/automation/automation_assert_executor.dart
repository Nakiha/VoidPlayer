import '../actions/player_assert.dart';
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
}
