import 'dart:io';

import '../app_log.dart';
import '../video_renderer_controller.dart';
import '../viewport/display_geometry.dart';
import 'automation_run_state.dart';

class AutomationProbe {
  final NativePlayerController controller;

  const AutomationProbe(this.controller);

  Future<ViewCenterMetric> currentViewCenterMetric() async {
    final layout = await controller.getLayout();
    final tracks = await controller.getTracks();
    final capture = await controller.captureViewport();
    final display = computeDisplayPixelSizeForLayout(
      viewportWidth: capture.width,
      viewportHeight: capture.height,
      layout: layout,
      tracks: tracks.map(DisplayTrackGeometry.fromTrackInfo).toList(),
    );
    final x = display.width.abs() > 1e-4
        ? layout.viewOffsetX / display.width
        : 0.0;
    final y = display.height.abs() > 1e-4
        ? layout.viewOffsetY / display.height
        : 0.0;
    return ViewCenterMetric(x, y);
  }

  Future<ResourceUsageMetric> currentResourceUsageMetric() async {
    final diagnostics = await controller.getDiagnostics();
    final rssBytes =
        diagnostics['processRssBytes'] as int? ?? ProcessInfo.currentRss;
    final privateBytes =
        diagnostics['processPrivateBytes'] as int? ?? ProcessInfo.currentRss;
    final dedicatedGpuBytes =
        diagnostics['dedicatedGpuUsageBytes'] as int? ?? 0;
    return ResourceUsageMetric(
      rssBytes: rssBytes,
      privateBytes: privateBytes,
      dedicatedGpuBytes: dedicatedGpuBytes,
    );
  }

  int currentNativeSeekCount() {
    final file = File(
      '${logConfig.logsDir}${Platform.pathSeparator}${logConfig.nativeLogFileName}',
    );
    if (!file.existsSync()) {
      throw StateError('Native log file not found: ${file.path}');
    }
    final text = file.readAsStringSync();
    return RegExp(
      RegExp.escape('[VideoRendererPlugin] seek:'),
    ).allMatches(text).length;
  }

  static double bytesToMb(int bytes) => bytes / 1024.0 / 1024.0;

  static String formatMb(int bytes) => bytesToMb(bytes).toStringAsFixed(1);

  static void assertResourceMetricAvailable(
    ResourceUsageMetric metric,
    double gpuThresholdMb,
  ) {
    if (gpuThresholdMb >= 0 && metric.dedicatedGpuBytes <= 0) {
      throw AssertionError('Dedicated GPU memory metric is unavailable');
    }
  }
}
