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

class ViewportSourceHit {
  final int fileId;
  final Offset sourceUv;

  const ViewportSourceHit({required this.fileId, required this.sourceUv});
}

class ViewportProjectedSourceRect {
  final Rect viewportRect;
  final Rect clipRect;

  const ViewportProjectedSourceRect({
    required this.viewportRect,
    required this.clipRect,
  });

  bool get isVisible => !clipRect.isEmpty && viewportRect.overlaps(clipRect);
}

class ViewportLayoutProjection {
  final int viewportWidth;
  final int viewportHeight;
  final int mode;
  final double splitPos;
  final List<DisplayTrackGeometry> orderedTracks;
  final Map<int, _TrackProjection> _tracksByFileId;

  const ViewportLayoutProjection._({
    required this.viewportWidth,
    required this.viewportHeight,
    required this.mode,
    required this.splitPos,
    required this.orderedTracks,
    required this._tracksByFileId,
  });

  bool get isValid => viewportWidth > 0 && viewportHeight > 0;

  ViewportSourceHit? hitTestPhysical(Offset physicalPosition) {
    final target = _displayTrackForGlobalUv(
      _globalUvFromPhysical(physicalPosition),
    );
    if (target == null) return null;
    final sourceUv = _sourceUvForLocalUv(target.track.fileId, target.localUv);
    if (sourceUv == null || !_sourceUvInBounds(sourceUv)) return null;
    return ViewportSourceHit(fileId: target.track.fileId, sourceUv: sourceUv);
  }

  Rect? sourceRectForDrag({
    required ViewportSourceHit start,
    required Offset endPhysicalPosition,
  }) {
    final end = sourceUvForTrackPhysical(
      start.fileId,
      endPhysicalPosition,
      clipToVisibleRegion: true,
    );
    if (end == null) return null;
    return _normalizedRectFromPoints(start.sourceUv, end);
  }

  Offset? sourceUvForTrackPhysical(
    int fileId,
    Offset physicalPosition, {
    bool clipToVisibleRegion = false,
  }) {
    final projection = _tracksByFileId[fileId];
    if (projection == null) return null;

    var globalUv = _globalUvFromPhysical(physicalPosition);
    if (clipToVisibleRegion) {
      globalUv = Offset(
        globalUv.dx.clamp(0.0, 1.0),
        globalUv.dy.clamp(0.0, 1.0),
      );
    }

    var localUv = _localUvForTrackGlobalUv(fileId, globalUv);
    if (localUv == null) return null;

    if (clipToVisibleRegion) {
      final visible = _visibleLocalRectForTrack(fileId);
      if (visible == null || visible.isEmpty) return null;
      localUv = Offset(
        localUv.dx.clamp(visible.left, visible.right),
        localUv.dy.clamp(visible.top, visible.bottom),
      );
    }

    final sourceUv = _sourceUvForLocalUv(fileId, localUv);
    if (sourceUv == null) return null;
    if (clipToVisibleRegion) {
      return Offset(sourceUv.dx.clamp(0.0, 1.0), sourceUv.dy.clamp(0.0, 1.0));
    }
    return sourceUv;
  }

  Rect? viewportRectForSourceRect(int fileId, Rect sourceRect) {
    final projected = viewportProjectionForSourceRect(fileId, sourceRect);
    if (projected == null || projected.clipRect.isEmpty) return null;
    final clipped = projected.viewportRect.intersect(projected.clipRect);
    if (clipped.isEmpty) return null;
    return clipped;
  }

  ViewportProjectedSourceRect? viewportProjectionForSourceRect(
    int fileId,
    Rect sourceRect,
  ) {
    final projection = _tracksByFileId[fileId];
    if (projection == null ||
        projection.invDisplaySizeX == 0.0 ||
        projection.invDisplaySizeY == 0.0) {
      return null;
    }

    final displaySizeX = 1.0 / projection.invDisplaySizeX;
    final displaySizeY = 1.0 / projection.invDisplaySizeY;
    final localMin = Offset(
      projection.displayOffsetX +
          (sourceRect.left + projection.viewOffsetUvX) * displaySizeX,
      projection.displayOffsetY +
          (sourceRect.top + projection.viewOffsetUvY) * displaySizeY,
    );
    final localMax = Offset(
      projection.displayOffsetX +
          (sourceRect.right + projection.viewOffsetUvX) * displaySizeX,
      projection.displayOffsetY +
          (sourceRect.bottom + projection.viewOffsetUvY) * displaySizeY,
    );
    final localRect = _normalizedRectFromPoints(localMin, localMax);
    final visible = _visibleLocalRectForTrack(fileId);
    if (visible == null || visible.isEmpty) return null;

    final fullViewportRect = _viewportRectForTrackLocalRect(fileId, localRect);
    final clipViewportRect = _viewportRectForTrackLocalRect(fileId, visible);
    if (fullViewportRect == null || clipViewportRect == null) return null;
    return ViewportProjectedSourceRect(
      viewportRect: fullViewportRect,
      clipRect: clipViewportRect.intersect(
        Rect.fromLTWH(
          0,
          0,
          viewportWidth.toDouble(),
          viewportHeight.toDouble(),
        ),
      ),
    );
  }

  _DisplayedTrack? _displayTrackForGlobalUv(Offset globalUv) {
    if (!isValid || orderedTracks.isEmpty) return null;
    if (mode == LayoutMode.splitScreen) {
      final slot = globalUv.dx < splitPos ? 0 : 1;
      if (slot < 0 || slot >= orderedTracks.length) return null;
      return _DisplayedTrack(track: orderedTracks[slot], localUv: globalUv);
    }

    final count = orderedTracks.length;
    final scaledX = globalUv.dx * count;
    final slot = scaledX.floor().clamp(0, count - 1);
    return _DisplayedTrack(
      track: orderedTracks[slot],
      localUv: Offset(scaledX - slot, globalUv.dy),
    );
  }

  Offset _globalUvFromPhysical(Offset physicalPosition) {
    if (!isValid) return Offset.zero;
    return Offset(
      physicalPosition.dx / viewportWidth,
      physicalPosition.dy / viewportHeight,
    );
  }

  Offset? _sourceUvForLocalUv(int fileId, Offset localUv) {
    final projection = _tracksByFileId[fileId];
    if (projection == null) return null;
    return Offset(
      (localUv.dx - projection.displayOffsetX) * projection.invDisplaySizeX -
          projection.viewOffsetUvX,
      (localUv.dy - projection.displayOffsetY) * projection.invDisplaySizeY -
          projection.viewOffsetUvY,
    );
  }

  Offset? _localUvForTrackGlobalUv(int fileId, Offset globalUv) {
    if (mode == LayoutMode.splitScreen) return globalUv;
    final slot = _displaySlotForTrack(fileId);
    if (slot < 0 || orderedTracks.isEmpty) return null;
    final count = orderedTracks.length;
    return Offset(globalUv.dx * count - slot, globalUv.dy);
  }

  Offset? _globalUvForTrackLocalUv(int fileId, Offset localUv) {
    if (mode == LayoutMode.splitScreen) return localUv;
    final slot = _displaySlotForTrack(fileId);
    if (slot < 0 || orderedTracks.isEmpty) return null;
    final count = orderedTracks.length;
    return Offset((slot + localUv.dx) / count, localUv.dy);
  }

  Rect? _visibleLocalRectForTrack(int fileId) {
    var visible = const Rect.fromLTRB(0.0, 0.0, 1.0, 1.0);
    if (mode != LayoutMode.splitScreen) return visible;
    final slot = _displaySlotForTrack(fileId);
    if (slot == 0) {
      visible = Rect.fromLTRB(0.0, 0.0, splitPos.clamp(0.0, 1.0), 1.0);
    } else if (slot == 1) {
      visible = Rect.fromLTRB(splitPos.clamp(0.0, 1.0), 0.0, 1.0, 1.0);
    } else {
      return Rect.zero;
    }
    return visible;
  }

  int _displaySlotForTrack(int fileId) =>
      orderedTracks.indexWhere((track) => track.fileId == fileId);

  Rect? _viewportRectForTrackLocalRect(int fileId, Rect localRect) {
    final topLeft = _globalUvForTrackLocalUv(fileId, localRect.topLeft);
    final bottomRight = _globalUvForTrackLocalUv(fileId, localRect.bottomRight);
    if (topLeft == null || bottomRight == null) return null;
    final globalRect = _normalizedRectFromPoints(topLeft, bottomRight);
    return Rect.fromLTRB(
      globalRect.left * viewportWidth,
      globalRect.top * viewportHeight,
      globalRect.right * viewportWidth,
      globalRect.bottom * viewportHeight,
    );
  }
}

ViewportLayoutProjection computeViewportLayoutProjection({
  required int viewportWidth,
  required int viewportHeight,
  required LayoutState layout,
  required List<DisplayTrackGeometry> tracks,
}) {
  final orderedTracks = _orderedTracksForLayout(layout, tracks);
  final activeCount = orderedTracks.length;
  final slotWidth = _slotWidthForLayout(viewportWidth, layout, activeCount);
  final slotHeight = viewportHeight.toDouble();
  final slotAspect = slotHeight > 0 ? slotWidth / slotHeight : 1.0;
  final tracksByFileId = <int, _TrackProjection>{};

  for (final track in tracks) {
    var videoAspect = track.height > 0
        ? track.width / track.height
        : slotAspect;
    if (!videoAspect.isFinite || videoAspect <= 0) videoAspect = slotAspect;

    var trackScale = 1.0;
    if (layout.pixelSizeMode == LayoutPixelSizeMode.uniformVideoPixels) {
      trackScale = _uniformPixelScaleForTrack(
        track,
        tracks,
        slotWidth,
        slotHeight,
      );
    }

    var fitScale = videoAspect > slotAspect ? slotAspect / videoAspect : 1.0;
    fitScale *= trackScale;
    if (!fitScale.isFinite || fitScale <= 0.0) fitScale = 1.0;

    var displayScale = fitScale * layout.zoomRatio;
    if (!displayScale.isFinite || displayScale <= 0.0) displayScale = 1.0;

    final dsX = slotAspect > 0
        ? videoAspect * displayScale / slotAspect
        : displayScale;
    final dsY = displayScale;
    final displayPixelWidth = dsX * slotWidth;
    final displayPixelHeight = dsY * slotHeight;
    tracksByFileId[track.fileId] = _TrackProjection(
      displayOffsetX: (1.0 - dsX) * 0.5,
      displayOffsetY: (1.0 - dsY) * 0.5,
      invDisplaySizeX: dsX.abs() > 1e-4 ? 1.0 / dsX : 0.0,
      invDisplaySizeY: dsY.abs() > 1e-4 ? 1.0 / dsY : 0.0,
      viewOffsetUvX: displayPixelWidth.abs() > 1e-4
          ? layout.viewOffsetX / displayPixelWidth
          : 0.0,
      viewOffsetUvY: displayPixelHeight.abs() > 1e-4
          ? layout.viewOffsetY / displayPixelHeight
          : 0.0,
    );
  }

  return ViewportLayoutProjection._(
    viewportWidth: viewportWidth,
    viewportHeight: viewportHeight,
    mode: layout.mode,
    splitPos: layout.splitPos,
    orderedTracks: orderedTracks,
    tracksByFileId: tracksByFileId,
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

class _TrackProjection {
  final double displayOffsetX;
  final double displayOffsetY;
  final double invDisplaySizeX;
  final double invDisplaySizeY;
  final double viewOffsetUvX;
  final double viewOffsetUvY;

  const _TrackProjection({
    required this.displayOffsetX,
    required this.displayOffsetY,
    required this.invDisplaySizeX,
    required this.invDisplaySizeY,
    required this.viewOffsetUvX,
    required this.viewOffsetUvY,
  });
}

class _DisplayedTrack {
  final DisplayTrackGeometry track;
  final Offset localUv;

  const _DisplayedTrack({required this.track, required this.localUv});
}

List<DisplayTrackGeometry> _orderedTracksForLayout(
  LayoutState layout,
  List<DisplayTrackGeometry> tracks,
) {
  final ordered = <DisplayTrackGeometry>[];
  for (final fileId in layout.order) {
    final index = tracks.indexWhere((track) => track.fileId == fileId);
    if (index >= 0 && !ordered.contains(tracks[index])) {
      ordered.add(tracks[index]);
    }
  }
  for (final track in tracks) {
    if (!ordered.contains(track)) ordered.add(track);
  }
  return ordered;
}

double _slotWidthForLayout(int width, LayoutState layout, int activeCount) {
  var slotWidth = width.toDouble();
  if (layout.mode != LayoutMode.splitScreen && activeCount > 1) {
    slotWidth /= activeCount;
  }
  return slotWidth;
}

double _uniformPixelScaleForTrack(
  DisplayTrackGeometry track,
  List<DisplayTrackGeometry> tracks,
  double slotWidth,
  double slotHeight,
) {
  var refTrack = track;
  var maxPixels = -1;
  for (final candidate in tracks) {
    final pixels = candidate.width * candidate.height;
    if (pixels > maxPixels) {
      maxPixels = pixels;
      refTrack = candidate;
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
  return trackDensity > 0 ? refDensity / trackDensity : 1.0;
}

bool _sourceUvInBounds(Offset uv) =>
    uv.dx >= 0.0 && uv.dx <= 1.0 && uv.dy >= 0.0 && uv.dy <= 1.0;

Rect _normalizedRectFromPoints(Offset a, Offset b) {
  return Rect.fromLTRB(
    math.min(a.dx, b.dx),
    math.min(a.dy, b.dy),
    math.max(a.dx, b.dx),
    math.max(a.dy, b.dy),
  );
}
