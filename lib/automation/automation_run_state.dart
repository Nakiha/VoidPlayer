import '../video_renderer_controller.dart';

class ViewCenterMetric {
  final double x;
  final double y;

  const ViewCenterMetric(this.x, this.y);
}

class ResourceUsageMetric {
  final int rssBytes;
  final int privateBytes;
  final int heapAllocatedBytes;
  final int heapCommittedBytes;
  final int heapReservedBytes;
  final int heapCount;
  final int dedicatedGpuBytes;

  const ResourceUsageMetric({
    required this.rssBytes,
    required this.privateBytes,
    required this.heapAllocatedBytes,
    required this.heapCommittedBytes,
    required this.heapReservedBytes,
    required this.heapCount,
    required this.dedicatedGpuBytes,
  });
}

class AutomationRunState {
  final captures = <String, ViewportCapture>{};
  final viewCenterBaselines = <String, ViewCenterMetric>{};
  final resourceBaselines = <String, ResourceUsageMetric>{};
  final nativeSeekCountBaselines = <String, int>{};
}
