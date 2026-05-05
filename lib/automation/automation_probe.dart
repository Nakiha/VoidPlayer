import 'dart:io';
import 'dart:math' as math;

import '../app_log.dart';
import '../video_renderer_controller.dart';
import 'automation_run_state.dart';

class AutomationProbe {
  final NativePlayerController controller;

  const AutomationProbe(this.controller);

  Future<ViewCenterMetric> currentViewCenterMetric() async {
    final layout = await controller.getLayout();
    final tracks = await controller.getTracks();
    final capture = await controller.captureViewport();
    final display = _displayPixelSizeForLayout(
      width: capture.width,
      height: capture.height,
      layout: layout,
      tracks: tracks,
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
    final dedicatedGpuBytes =
        diagnostics['dedicatedGpuUsageBytes'] as int? ?? 0;
    return ResourceUsageMetric(
      rssBytes: rssBytes,
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

  ({double width, double height}) _displayPixelSizeForLayout({
    required int width,
    required int height,
    required LayoutState layout,
    required List<TrackInfo> tracks,
  }) {
    if (width <= 0 || height <= 0 || tracks.isEmpty) {
      return (width: width.toDouble(), height: height.toDouble());
    }

    TrackInfo? track;
    for (final fileId in layout.order) {
      for (final candidate in tracks) {
        if (candidate.fileId == fileId) {
          track = candidate;
          break;
        }
      }
      if (track != null) break;
    }
    track ??= tracks.first;

    var slotW = width.toDouble();
    final slotH = height.toDouble();
    if (layout.mode != LayoutMode.splitScreen && tracks.length > 1) {
      slotW /= tracks.length;
    }
    final slotAspect = slotH > 0 ? slotW / slotH : 1.0;

    var refTrack = tracks.first;
    var maxPixels = 0;
    for (final candidate in tracks) {
      final pixels = candidate.width * candidate.height;
      if (pixels > maxPixels) {
        maxPixels = pixels;
        refTrack = candidate;
      }
    }

    final refW = refTrack.width.toDouble();
    final refH = refTrack.height.toDouble();
    final refDensity = refW > 0 && refH > 0
        ? math.min(slotW / refW, slotH / refH)
        : 1.0;

    final trackW = track.width.toDouble();
    final trackH = track.height.toDouble();
    final trackDensity = trackW > 0 && trackH > 0
        ? math.min(slotW / trackW, slotH / trackH)
        : 1.0;
    final trackScale = trackDensity > 0 ? refDensity / trackDensity : 1.0;

    var videoAspect = trackH > 0 ? trackW / trackH : slotAspect;
    if (videoAspect <= 0) videoAspect = slotAspect;

    var fitScale = videoAspect > slotAspect ? slotAspect / videoAspect : 1.0;
    fitScale *= trackScale;
    final displayScale = fitScale * layout.zoomRatio;
    final dsX = slotAspect > 0
        ? videoAspect * displayScale / slotAspect
        : displayScale;
    final dsY = displayScale;

    return (width: dsX * slotW, height: dsY * slotH);
  }
}
