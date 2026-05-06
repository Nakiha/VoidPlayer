import 'dart:math' as math;
import 'dart:ui';

import '../video_renderer_controller.dart';

class DisplayTrackGeometry {
  final int fileId;
  final int width;
  final int height;

  const DisplayTrackGeometry({
    required this.fileId,
    required this.width,
    required this.height,
  });

  factory DisplayTrackGeometry.fromTrackInfo(TrackInfo info) =>
      DisplayTrackGeometry(
        fileId: info.fileId,
        width: info.width,
        height: info.height,
      );
}

Size computeDisplayPixelSizeForLayout({
  required int viewportWidth,
  required int viewportHeight,
  required LayoutState layout,
  required List<DisplayTrackGeometry> tracks,
}) {
  if (viewportWidth <= 0 || viewportHeight <= 0) return Size.zero;
  if (tracks.isEmpty) {
    return Size(viewportWidth.toDouble(), viewportHeight.toDouble());
  }

  var track = tracks.first;
  for (final fileId in layout.order) {
    final index = tracks.indexWhere((entry) => entry.fileId == fileId);
    if (index >= 0) {
      track = tracks[index];
      break;
    }
  }

  var slotWidth = viewportWidth.toDouble();
  final slotHeight = viewportHeight.toDouble();
  if (layout.mode != LayoutMode.splitScreen && tracks.length > 1) {
    slotWidth /= tracks.length;
  }
  final slotAspect = slotHeight > 0 ? slotWidth / slotHeight : 1.0;

  var trackScale = 1.0;
  if (layout.pixelSizeMode == LayoutPixelSizeMode.uniformVideoPixels) {
    var refTrack = tracks.first;
    var maxPixels = 0;
    for (final entry in tracks) {
      final pixels = entry.width * entry.height;
      if (pixels > maxPixels) {
        maxPixels = pixels;
        refTrack = entry;
      }
    }

    double densityFor(DisplayTrackGeometry entry) {
      final videoWidth = entry.width.toDouble();
      final videoHeight = entry.height.toDouble();
      if (videoWidth <= 0 || videoHeight <= 0) return 1.0;
      return math.min(slotWidth / videoWidth, slotHeight / videoHeight);
    }

    final trackDensity = densityFor(track);
    final refDensity = densityFor(refTrack);
    trackScale = trackDensity > 0 ? refDensity / trackDensity : 1.0;
  }

  final videoWidth = track.width.toDouble();
  final videoHeight = track.height.toDouble();
  var videoAspect = videoHeight > 0 ? videoWidth / videoHeight : slotAspect;
  if (videoAspect <= 0) videoAspect = slotAspect;

  var fitScale = videoAspect > slotAspect ? slotAspect / videoAspect : 1.0;
  fitScale *= trackScale;
  final displayScale = fitScale * layout.zoomRatio;
  final dsX = slotAspect > 0
      ? videoAspect * displayScale / slotAspect
      : displayScale;
  final dsY = displayScale;
  return Size(dsX * slotWidth, dsY * slotHeight);
}
