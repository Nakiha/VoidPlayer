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
    final heapAllocatedBytes =
        diagnostics['processHeapAllocatedBytes'] as int? ?? 0;
    final heapCommittedBytes =
        diagnostics['processHeapCommittedBytes'] as int? ?? 0;
    final heapReservedBytes =
        diagnostics['processHeapReservedBytes'] as int? ?? 0;
    final heapCount = diagnostics['processHeapCount'] as int? ?? 0;
    final dedicatedGpuBytes =
        diagnostics['dedicatedGpuUsageBytes'] as int? ?? 0;
    final gpuBreakdown =
        diagnostics['gpuBreakdown'] as Map<dynamic, dynamic>? ?? const {};
    return ResourceUsageMetric(
      rssBytes: rssBytes,
      privateBytes: privateBytes,
      heapAllocatedBytes: heapAllocatedBytes,
      heapCommittedBytes: heapCommittedBytes,
      heapReservedBytes: heapReservedBytes,
      heapCount: heapCount,
      dedicatedGpuBytes: dedicatedGpuBytes,
      gpuBreakdown: gpuBreakdown.map((key, value) => MapEntry('$key', value)),
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

  static String formatGpuBreakdown(
    Map<String, dynamic> breakdown, {
    int dedicatedGpuBytes = 0,
  }) {
    if (breakdown.isEmpty) {
      return 'gpuBreakdown=unavailable';
    }

    int bytes(String key) => breakdown[key] as int? ?? 0;
    final knownBytes = bytes('totalEstimatedBytes');
    final unattributedBytes = dedicatedGpuBytes > knownBytes
        ? dedicatedGpuBytes - knownBytes
        : 0;
    final parts = <String>[
      'known=${formatMb(knownBytes)}MB',
      'unattributed=${formatMb(unattributedBytes)}MB',
      'decoderPool=${formatMb(bytes('decoderPoolBytes'))}MB',
      'presenter=${formatMb(bytes('presenterTextureBytes'))}MB',
      'headless=${formatMb(bytes('headlessOutputBytes'))}MB',
      'seekSnapshot=${formatMb(bytes('exactSeekSnapshotBytes'))}MB',
      'overlay=${formatMb(bytes('analysisOverlayBytes'))}MB',
    ];

    final tracks = breakdown['tracks'] as List<dynamic>? ?? const [];
    for (final rawTrack in tracks) {
      final track = rawTrack as Map<dynamic, dynamic>? ?? const {};
      final slot = track['slot'] ?? '?';
      final pool = track['decoderPoolBytes'] as int? ?? 0;
      final frame = track['decoderFrameBytes'] as int? ?? 0;
      final poolSize = track['hwInitialPoolSize'] ?? 0;
      final copy = track['presenterCopyTextureBytes'] as int? ?? 0;
      final snapshots = track['exactSeekSnapshotBytes'] as int? ?? 0;
      final hwWidth = track['hwWidth'] ?? 0;
      final hwHeight = track['hwHeight'] ?? 0;
      parts.add(
        't$slot=${hwWidth}x$hwHeight pool=${formatMb(pool)}MB'
        '/$poolSize frame=${formatMb(frame)}MB '
        'copy=${formatMb(copy)}MB snap=${formatMb(snapshots)}MB',
      );
    }
    return 'gpuBreakdown=${parts.join('; ')}';
  }

  static void assertResourceMetricAvailable(
    ResourceUsageMetric metric,
    double gpuThresholdMb,
  ) {
    if (gpuThresholdMb >= 0 && metric.dedicatedGpuBytes <= 0) {
      throw AssertionError('Dedicated GPU memory metric is unavailable');
    }
  }
}
