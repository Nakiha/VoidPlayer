import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import '../../utils/async_guard.dart';
import '../../track_manager.dart';
import '../../video_renderer_controller.dart';

class MainWindowLayoutCoordinator {
  static const Duration viewportResizeDebounce = Duration(milliseconds: 80);
  static const double timelineTrackRowLogicalHeight = 40.0;

  final TickerProvider vsync;
  final NativePlayerController controller;
  final bool Function() mounted;
  final int? Function() textureId;
  final LayoutState Function() layout;
  final void Function(LayoutState layout) setLayout;
  final int Function() trackCount;
  final List<TrackEntry> Function() tracks;

  Ticker? _ticker;
  Timer? _resizeDebounceTimer;
  bool _layoutDirty = false;
  bool _resizeDirty = false;
  bool _flushInProgress = false;
  bool _disposed = false;
  int _viewportResizeSuppressionDepth = 0;

  int viewportWidth = 0;
  int viewportHeight = 0;
  double viewportDevicePixelRatio = 1.0;

  MainWindowLayoutCoordinator({
    required this.vsync,
    required this.controller,
    required this.mounted,
    required this.textureId,
    required this.layout,
    required this.setLayout,
    required this.trackCount,
    required this.tracks,
  }) {
    _ticker = vsync.createTicker((_) {
      fireAndLog('flush pending layout', flushPendingLayout());
    });
  }

  void dispose() {
    _disposed = true;
    _resizeDebounceTimer?.cancel();
    _ticker?.dispose();
    _ticker = null;
  }

  void toggleLayoutMode() {
    if (_disposed) return;
    setLayoutMode(
      layout().mode == LayoutMode.sideBySide
          ? LayoutMode.splitScreen
          : LayoutMode.sideBySide,
    );
  }

  void setLayoutMode(int mode) {
    if (_disposed) return;
    _updateLayout((layout) => layout.copyWith(mode: mode));
    markLayoutDirty();
  }

  void setZoom(double ratio) {
    if (_disposed) return;
    _updateLayout(
      (layout) => layout.copyWith(
        zoomRatio: ratio.clamp(LayoutState.zoomMin, LayoutState.zoomMax),
      ),
    );
    markLayoutDirty();
  }

  void setSplitPos(double pos) {
    if (_disposed) return;
    _updateLayout((layout) => layout.copyWith(splitPos: pos.clamp(0.0, 1.0)));
    markLayoutDirty();
  }

  void panByDelta(double dx, double dy) {
    if (_disposed) return;
    _updateLayout(
      (layout) => layout.copyWith(
        viewOffsetX: layout.viewOffsetX + dx,
        viewOffsetY: layout.viewOffsetY + dy,
      ),
    );
    markLayoutDirty();
  }

  void onPan(Offset delta) {
    panByDelta(delta.dx, delta.dy);
  }

  void onSplit(double normalizedX) {
    setSplitPos(normalizedX);
  }

  void onZoom(double scrollDelta, Offset localPos) {
    if (_disposed) return;
    final currentLayout = layout();
    final factor = scrollDelta > 0 ? 0.9 : 1.1;
    final newZoom = (currentLayout.zoomRatio * factor).clamp(
      LayoutState.zoomMin,
      LayoutState.zoomMax,
    );

    if (newZoom == LayoutState.zoomMin && factor < 1.0) {
      _updateLayout(
        (layout) =>
            layout.copyWith(zoomRatio: newZoom, viewOffsetX: 0, viewOffsetY: 0),
      );
      markLayoutDirty();
      return;
    }

    final actualFactor = newZoom / currentLayout.zoomRatio;

    if (viewportWidth <= 0 || viewportHeight <= 0) {
      _updateLayout((layout) => layout.copyWith(zoomRatio: newZoom));
      markLayoutDirty();
      return;
    }

    double cursorX, cursorY, slotW, slotH;
    if (currentLayout.mode == LayoutMode.sideBySide) {
      final n = trackCount() > 0 ? trackCount() : 1;
      final nx = localPos.dx / viewportWidth;
      final ny = localPos.dy / viewportHeight;
      final slotIndex = (nx * n).floor().clamp(0, n - 1);
      cursorX = nx * n - slotIndex;
      cursorY = ny;
      slotW = viewportWidth / n;
      slotH = viewportHeight.toDouble();
    } else {
      cursorX = localPos.dx / viewportWidth;
      cursorY = localPos.dy / viewportHeight;
      slotW = viewportWidth.toDouble();
      slotH = viewportHeight.toDouble();
    }

    _updateLayout(
      (layout) => layout.copyWith(
        zoomRatio: newZoom,
        viewOffsetX:
            actualFactor * layout.viewOffsetX +
            (1 - actualFactor) * (cursorX - 0.5) * slotW,
        viewOffsetY:
            actualFactor * layout.viewOffsetY +
            (1 - actualFactor) * (cursorY - 0.5) * slotH,
      ),
    );
    markLayoutDirty();
  }

  void onPointerButton(bool panning, bool splitting) {
    // Reserved for cursor or mode hints.
  }

  void onViewportResize(int width, int height, double devicePixelRatio) {
    if (_disposed) return;
    if (devicePixelRatio > 0) {
      viewportDevicePixelRatio = devicePixelRatio;
    }
    if (_viewportResizeSuppressionDepth > 0) return;
    if (width == viewportWidth && height == viewportHeight) return;
    final previousWidth = viewportWidth;
    final previousHeight = viewportHeight;
    if (previousWidth > 0 && previousHeight > 0) {
      _rescaleViewOffsetForResize(previousWidth, previousHeight, width, height);
    }
    viewportWidth = width;
    viewportHeight = height;
    _resizeDebounceTimer?.cancel();
    _resizeDebounceTimer = Timer(viewportResizeDebounce, () {
      if (_disposed || !mounted()) return;
      _markResizeDirty();
    });
  }

  Future<void> preemptTimelineTrackCountChange({
    required int previousCount,
    required int nextCount,
  }) async {
    if (_disposed || textureId() == null) return;
    if (previousCount <= 0 || nextCount <= 0 || previousCount == nextCount) {
      return;
    }
    if (viewportWidth <= 0 || viewportHeight <= 0) return;

    final rowDelta = nextCount - previousCount;
    final heightDelta =
        (rowDelta * timelineTrackRowLogicalHeight * viewportDevicePixelRatio)
            .round();
    if (heightDelta == 0) return;

    final nextHeight = (viewportHeight - heightDelta).clamp(1, 1 << 30).toInt();
    if (nextHeight == viewportHeight) return;

    await preemptViewportResize(width: viewportWidth, height: nextHeight);
  }

  Future<void> preemptViewportResize({
    required int width,
    required int height,
  }) async {
    if (_disposed || textureId() == null) return;
    if (width <= 0 || height <= 0) return;
    if (width == viewportWidth && height == viewportHeight) return;

    final previousWidth = viewportWidth;
    final previousHeight = viewportHeight;
    _resizeDebounceTimer?.cancel();
    _resizeDebounceTimer = null;
    _resizeDirty = false;
    if (previousWidth > 0 && previousHeight > 0) {
      _rescaleViewOffsetForResize(previousWidth, previousHeight, width, height);
    }
    viewportWidth = width;
    viewportHeight = height;
    await controller.resize(width, height);
  }

  void beginViewportResizeSuppression() {
    if (_disposed) return;
    _viewportResizeSuppressionDepth++;
    _resizeDebounceTimer?.cancel();
    _resizeDebounceTimer = null;
    _resizeDirty = false;
  }

  void endViewportResizeSuppression() {
    if (_viewportResizeSuppressionDepth <= 0) return;
    _viewportResizeSuppressionDepth--;
  }

  void onZoomComboChanged(double value) {
    if (_disposed) return;
    _updateLayout((layout) => layout.copyWith(zoomRatio: value));
    markLayoutDirty();
  }

  void markLayoutDirty() {
    if (_disposed) return;
    _layoutDirty = true;
    _ticker?.start();
  }

  void _markResizeDirty() {
    if (_disposed) return;
    _resizeDirty = true;
    _ticker?.start();
  }

  Future<void> flushPendingLayout() async {
    if (_disposed || _flushInProgress) return;
    if (textureId() == null) {
      _resizeDirty = false;
      _layoutDirty = false;
      _ticker?.stop();
      return;
    }

    _flushInProgress = true;
    try {
      while (!_disposed && mounted() && (_resizeDirty || _layoutDirty)) {
        if (_resizeDirty && viewportWidth > 0 && viewportHeight > 0) {
          final width = viewportWidth;
          final height = viewportHeight;
          _resizeDirty = false;
          await controller.resize(width, height);
          if (_disposed || !mounted()) return;
          if (!_layoutDirty) {
            final nextLayout = await controller.getLayout();
            if (_disposed || !mounted()) return;
            setLayout(nextLayout);
          }
        } else if (_resizeDirty) {
          _resizeDirty = false;
        }

        if (_layoutDirty) {
          final nextLayout = layout();
          _layoutDirty = false;
          await controller.applyLayout(nextLayout);
          if (_disposed || !mounted()) return;
        }
      }
    } finally {
      _flushInProgress = false;
      if (!_disposed && mounted()) {
        if (_resizeDirty || _layoutDirty) {
          _ticker?.start();
        } else {
          _ticker?.stop();
        }
      }
    }
  }

  void _updateLayout(LayoutState Function(LayoutState current) update) {
    setLayout(update(layout()));
  }

  void _rescaleViewOffsetForResize(
    int oldWidth,
    int oldHeight,
    int newWidth,
    int newHeight,
  ) {
    final current = layout();
    final oldDisplay = _displayPixelSizeForLayout(oldWidth, oldHeight, current);
    final newDisplay = _displayPixelSizeForLayout(newWidth, newHeight, current);
    if (oldDisplay == Size.zero || newDisplay == Size.zero) return;

    var nextOffsetX = current.viewOffsetX;
    var nextOffsetY = current.viewOffsetY;
    if (oldDisplay.width.abs() > 1e-4 && newDisplay.width.abs() > 1e-4) {
      nextOffsetX *= newDisplay.width / oldDisplay.width;
    }
    if (oldDisplay.height.abs() > 1e-4 && newDisplay.height.abs() > 1e-4) {
      nextOffsetY *= newDisplay.height / oldDisplay.height;
    }
    if (nextOffsetX == current.viewOffsetX &&
        nextOffsetY == current.viewOffsetY) {
      return;
    }
    _updateLayout(
      (layout) =>
          layout.copyWith(viewOffsetX: nextOffsetX, viewOffsetY: nextOffsetY),
    );
  }

  Size _displayPixelSizeForLayout(int width, int height, LayoutState layout) {
    if (width <= 0 || height <= 0) return Size.zero;
    final activeTracks = tracks();
    if (activeTracks.isEmpty) {
      return Size(width.toDouble(), height.toDouble());
    }

    TrackEntry track = activeTracks.first;
    for (final fileId in layout.order) {
      final index = activeTracks.indexWhere((entry) => entry.fileId == fileId);
      if (index >= 0) {
        track = activeTracks[index];
        break;
      }
    }

    var slotWidth = width.toDouble();
    final slotHeight = height.toDouble();
    if (layout.mode != LayoutMode.splitScreen && activeTracks.length > 1) {
      slotWidth /= activeTracks.length;
    }
    final slotAspect = slotHeight > 0 ? slotWidth / slotHeight : 1.0;

    var refTrack = activeTracks.first;
    var maxPixels = 0;
    for (final entry in activeTracks) {
      final pixels = entry.info.width * entry.info.height;
      if (pixels > maxPixels) {
        maxPixels = pixels;
        refTrack = entry;
      }
    }

    double densityFor(TrackEntry entry) {
      final videoWidth = entry.info.width.toDouble();
      final videoHeight = entry.info.height.toDouble();
      if (videoWidth <= 0 || videoHeight <= 0) return 1.0;
      return (slotWidth / videoWidth) < (slotHeight / videoHeight)
          ? slotWidth / videoWidth
          : slotHeight / videoHeight;
    }

    final trackDensity = densityFor(track);
    final refDensity = densityFor(refTrack);
    final trackScale = trackDensity > 0 ? refDensity / trackDensity : 1.0;

    final videoWidth = track.info.width.toDouble();
    final videoHeight = track.info.height.toDouble();
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
}
