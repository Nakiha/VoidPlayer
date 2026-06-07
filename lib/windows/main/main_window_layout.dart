import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import '../../marks/quick_mark.dart';
import '../../track_manager.dart';
import '../../utils/async_guard.dart';
import '../../video_renderer_controller.dart';
import '../../viewport/display_geometry.dart';
import 'main_window_state.dart';

class MainWindowLayoutCoordinator {
  static const Duration viewportResizeDebounce = Duration(milliseconds: 80);
  static const double timelineTrackRowLogicalHeight = 40.0;

  final TickerProvider vsync;
  final NativePlayerController controller;
  final MainWindowStateStore stateStore;
  final TrackManager trackManager;
  final bool Function() mounted;

  Ticker? _ticker;
  Timer? _resizeDebounceTimer;
  bool _layoutDirty = false;
  bool _resizeDirty = false;
  bool _flushInProgress = false;
  bool _disposed = false;
  Future<void>? _preemptResizeFuture;
  int? _queuedPreemptWidth;
  int? _queuedPreemptHeight;

  int viewportWidth = 0;
  int viewportHeight = 0;
  double viewportDevicePixelRatio = 1.0;

  MainWindowStateModel get _state => stateStore.value;

  int? textureId() => _state.textureId;
  LayoutState layout() => _state.layout;
  void setLayout(LayoutState layout) => stateStore.setLayout(layout);
  int trackCount() => trackManager.count;
  List<TrackEntry> tracks() => trackManager.entries;

  MainWindowLayoutCoordinator({
    required this.vsync,
    required this.controller,
    required this.stateStore,
    required this.trackManager,
    required this.mounted,
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
    final current = layout();
    if (current.mode == mode) return;
    final next = _rescaleViewOffsetForLayoutChange(
      current,
      current.copyWith(mode: mode),
    );
    setLayout(next);
    markLayoutDirty();
  }

  void setPixelSizeMode(int mode) {
    if (_disposed) return;
    final current = layout();
    if (current.pixelSizeMode == mode) return;
    final next = _rescaleViewOffsetForLayoutChange(
      current,
      current.copyWith(pixelSizeMode: mode),
    );
    setLayout(next);
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
    final current = layout();
    final nextOffsetX = current.viewOffsetX + dx;
    final nextOffsetY = current.viewOffsetY + dy;
    _updateLayout(
      (layout) =>
          layout.copyWith(viewOffsetX: nextOffsetX, viewOffsetY: nextOffsetY),
    );
    markLayoutDirty();
  }

  void onPan(Offset delta) {
    panByDelta(delta.dx, delta.dy);
  }

  void onSplit(double normalizedX) {
    setSplitPos(normalizedX);
  }

  void onZoom(double factor, Offset localPos) {
    if (_disposed) return;
    if (factor <= 0 || !factor.isFinite || factor == 1.0) return;
    final currentLayout = layout();
    final newZoom = (currentLayout.zoomRatio * factor).clamp(
      LayoutState.zoomMin,
      LayoutState.zoomMax,
    );
    if (newZoom == currentLayout.zoomRatio) return;

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

    final nextOffsetX =
        actualFactor * currentLayout.viewOffsetX +
        (1 - actualFactor) * (cursorX - 0.5) * slotW;
    final nextOffsetY =
        actualFactor * currentLayout.viewOffsetY +
        (1 - actualFactor) * (cursorY - 0.5) * slotH;

    _updateLayout(
      (layout) => layout.copyWith(
        zoomRatio: newZoom,
        viewOffsetX: nextOffsetX,
        viewOffsetY: nextOffsetY,
      ),
    );
    markLayoutDirty();
  }

  void onPointerButton(bool panning, bool splitting) {
    // Reserved for cursor or mode hints.
  }

  void onViewportResize(
    int width,
    int height,
    double devicePixelRatio, {
    bool immediate = false,
  }) {
    if (_disposed) return;
    if (devicePixelRatio > 0) {
      viewportDevicePixelRatio = devicePixelRatio;
    }
    if (width == viewportWidth && height == viewportHeight) return;
    final previousWidth = viewportWidth;
    final previousHeight = viewportHeight;
    if (!_layoutDirty && previousWidth > 0 && previousHeight > 0) {
      _rescaleViewOffsetForResize(previousWidth, previousHeight, width, height);
    }
    viewportWidth = width;
    viewportHeight = height;
    _resizeDebounceTimer?.cancel();
    if (immediate) {
      _resizeDebounceTimer = null;
      _markResizeDirty();
      return;
    }
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
    if (_layoutDirty) {
      final pendingLayout = layout();
      _layoutDirty = false;
      await controller.applyLayout(pendingLayout);
      if (_disposed || !mounted()) return;
    }
    if (previousWidth > 0 && previousHeight > 0) {
      _rescaleViewOffsetForResize(previousWidth, previousHeight, width, height);
    }
    viewportWidth = width;
    viewportHeight = height;
    await controller.resize(width, height);
  }

  void requestPreemptViewportLogicalSizeDelta({
    double widthDelta = 0,
    double heightDelta = 0,
  }) {
    if (_disposed || textureId() == null) return;
    if (viewportWidth <= 0 || viewportHeight <= 0) return;
    if (widthDelta == 0 && heightDelta == 0) return;
    final dpr = viewportDevicePixelRatio > 0 ? viewportDevicePixelRatio : 1.0;
    final baseWidth = _queuedPreemptWidth ?? viewportWidth;
    final baseHeight = _queuedPreemptHeight ?? viewportHeight;
    final nextWidth = (baseWidth + widthDelta * dpr)
        .round()
        .clamp(1, 1 << 30)
        .toInt();
    final nextHeight = (baseHeight + heightDelta * dpr)
        .round()
        .clamp(1, 1 << 30)
        .toInt();
    if (nextWidth == baseWidth && nextHeight == baseHeight) return;
    _queuedPreemptWidth = nextWidth;
    _queuedPreemptHeight = nextHeight;
    if (_preemptResizeFuture == null) {
      _preemptResizeFuture = _drainPreemptViewportResizeQueue();
      fireAndLog('preempt queued viewport resize', _preemptResizeFuture!);
    }
  }

  Future<void> _drainPreemptViewportResizeQueue() async {
    try {
      while (!_disposed && mounted()) {
        final width = _queuedPreemptWidth;
        final height = _queuedPreemptHeight;
        if (width == null || height == null) break;
        _queuedPreemptWidth = null;
        _queuedPreemptHeight = null;
        await preemptViewportResize(width: width, height: height);
      }
    } finally {
      _preemptResizeFuture = null;
      if (!_disposed &&
          mounted() &&
          _queuedPreemptWidth != null &&
          _queuedPreemptHeight != null) {
        _preemptResizeFuture = _drainPreemptViewportResizeQueue();
        fireAndLog('preempt queued viewport resize', _preemptResizeFuture!);
      }
    }
  }

  void onZoomComboChanged(double value) {
    setZoom(value);
  }

  void focusQuickMark(QuickMark mark) {
    if (_disposed) return;
    if (viewportWidth <= 0 || viewportHeight <= 0 || trackManager.isEmpty) {
      return;
    }
    final trackGeometry = tracks()
        .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry.info))
        .toList();
    final targetTrack = _trackGeometryByFileId(trackGeometry, mark.fileId);
    if (targetTrack == null) return;

    final current = layout();
    final orderedTracks = _orderedTracksForFocus(current, trackGeometry);
    final slotIndex = orderedTracks.indexWhere(
      (track) => track.fileId == mark.fileId,
    );
    if (slotIndex < 0) return;

    final activeCount = orderedTracks.length;
    final slotWidth = current.mode == LayoutMode.splitScreen || activeCount <= 1
        ? viewportWidth.toDouble()
        : viewportWidth / activeCount;
    final slotHeight = viewportHeight.toDouble();
    if (slotWidth <= 0 || slotHeight <= 0) return;

    final visibleLocal = _visibleLocalRectForFocus(current, slotIndex);
    if (visibleLocal.isEmpty) return;
    final visibleWidth = slotWidth * visibleLocal.width;
    final visibleHeight = slotHeight * visibleLocal.height;
    if (visibleWidth <= 0 || visibleHeight <= 0) return;

    final sourceBounds = _focusSourceBounds(mark);
    final sourceAnchor = mark.shape == QuickMarkShape.arrow
        ? mark.effectiveSourceEnd
        : sourceBounds.center;
    final baseDisplay = _displayPixelSizeForTrackAtZoom(
      track: targetTrack,
      tracks: trackGeometry,
      layout: current.copyWith(zoomRatio: 1.0, viewOffsetX: 0, viewOffsetY: 0),
      slotWidth: slotWidth,
      slotHeight: slotHeight,
    );
    if (baseDisplay.width <= 0 || baseDisplay.height <= 0) return;

    const targetFill = 0.55;
    final zoomCandidates = <double>[];
    final sourceWidth = math.max(sourceBounds.width, 0.04);
    final sourceHeight = math.max(sourceBounds.height, 0.04);
    zoomCandidates.add(
      visibleWidth * targetFill / (baseDisplay.width * sourceWidth),
    );
    zoomCandidates.add(
      visibleHeight * targetFill / (baseDisplay.height * sourceHeight),
    );
    final targetZoom = zoomCandidates
        .where((value) => value.isFinite && value > 0)
        .fold<double>(LayoutState.zoomMax, math.min)
        .clamp(LayoutState.zoomMin, LayoutState.zoomMax)
        .toDouble();

    final targetDisplay = _displayPixelSizeForTrackAtZoom(
      track: targetTrack,
      tracks: trackGeometry,
      layout: current.copyWith(
        zoomRatio: targetZoom,
        viewOffsetX: 0,
        viewOffsetY: 0,
      ),
      slotWidth: slotWidth,
      slotHeight: slotHeight,
    );
    if (targetDisplay.width <= 0 || targetDisplay.height <= 0) return;

    final displaySizeX = targetDisplay.width / slotWidth;
    final displaySizeY = targetDisplay.height / slotHeight;
    final displayOffsetX = (1.0 - displaySizeX) * 0.5;
    final displayOffsetY = (1.0 - displaySizeY) * 0.5;
    final desiredLocal = visibleLocal.center;
    final nextOffsetX =
        (desiredLocal.dx - displayOffsetX - sourceAnchor.dx * displaySizeX) *
        slotWidth;
    final nextOffsetY =
        (desiredLocal.dy - displayOffsetY - sourceAnchor.dy * displaySizeY) *
        slotHeight;

    setLayout(
      current.copyWith(
        zoomRatio: targetZoom,
        viewOffsetX: nextOffsetX,
        viewOffsetY: nextOffsetY,
      ),
    );
    markLayoutDirty();
  }

  void markLayoutDirty() {
    if (_disposed) return;
    _layoutDirty = true;
    _startTicker();
  }

  void _markResizeDirty() {
    if (_disposed) return;
    _resizeDirty = true;
    _startTicker();
  }

  void _startTicker() {
    final ticker = _ticker;
    if (ticker == null || ticker.isActive) return;
    ticker.start();
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
          if (_layoutDirty) {
            final pendingLayout = layout();
            _layoutDirty = false;
            await controller.applyLayout(pendingLayout);
            if (_disposed || !mounted()) return;
          }
          await controller.resize(width, height);
          if (_disposed || !mounted()) return;
          final nextLayout = await controller.getLayout();
          if (_disposed || !mounted()) return;
          setLayout(nextLayout);
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
          _startTicker();
        } else {
          _ticker?.stop();
        }
      }
    }
  }

  void _updateLayout(LayoutState Function(LayoutState current) update) {
    setLayout(update(layout()));
  }

  List<DisplayTrackGeometry> _orderedTracksForFocus(
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

  DisplayTrackGeometry? _trackGeometryByFileId(
    List<DisplayTrackGeometry> tracks,
    int fileId,
  ) {
    for (final track in tracks) {
      if (track.fileId == fileId) return track;
    }
    return null;
  }

  Rect _visibleLocalRectForFocus(LayoutState layout, int slotIndex) {
    if (layout.mode != LayoutMode.splitScreen) {
      return const Rect.fromLTRB(0.0, 0.0, 1.0, 1.0);
    }
    final split = layout.splitPos.clamp(0.0, 1.0).toDouble();
    if (slotIndex == 0) return Rect.fromLTRB(0.0, 0.0, split, 1.0);
    if (slotIndex == 1) return Rect.fromLTRB(split, 0.0, 1.0, 1.0);
    return Rect.zero;
  }

  Rect _focusSourceBounds(QuickMark mark) {
    switch (mark.shape) {
      case QuickMarkShape.rectangle:
        return mark.sourceRect;
      case QuickMarkShape.arrow:
        return Rect.fromPoints(
          mark.effectiveSourceStart,
          mark.effectiveSourceEnd,
        );
    }
  }

  Size _displayPixelSizeForTrackAtZoom({
    required DisplayTrackGeometry track,
    required List<DisplayTrackGeometry> tracks,
    required LayoutState layout,
    required double slotWidth,
    required double slotHeight,
  }) {
    final slotAspect = slotHeight > 0 ? slotWidth / slotHeight : 1.0;
    var videoAspect = track.height > 0
        ? track.width / track.height
        : slotAspect;
    if (!videoAspect.isFinite || videoAspect <= 0) videoAspect = slotAspect;

    var trackScale = 1.0;
    if (layout.pixelSizeMode == LayoutPixelSizeMode.uniformVideoPixels) {
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
      trackScale = trackDensity > 0 ? refDensity / trackDensity : 1.0;
    }

    var fitScale = videoAspect > slotAspect ? slotAspect / videoAspect : 1.0;
    fitScale *= trackScale;
    if (!fitScale.isFinite || fitScale <= 0.0) fitScale = 1.0;
    final displayScale = fitScale * layout.zoomRatio;
    final dsX = slotAspect > 0
        ? videoAspect * displayScale / slotAspect
        : displayScale;
    final dsY = displayScale;
    return Size(dsX * slotWidth, dsY * slotHeight);
  }

  LayoutState _rescaleViewOffsetForLayoutChange(
    LayoutState oldLayout,
    LayoutState newLayout,
  ) {
    if (viewportWidth <= 0 || viewportHeight <= 0) return newLayout;

    final trackGeometry = tracks()
        .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry.info))
        .toList();
    final oldDisplay = computeDisplayPixelSizeForLayout(
      viewportWidth: viewportWidth,
      viewportHeight: viewportHeight,
      layout: oldLayout,
      tracks: trackGeometry,
    );
    final newDisplay = computeDisplayPixelSizeForLayout(
      viewportWidth: viewportWidth,
      viewportHeight: viewportHeight,
      layout: newLayout,
      tracks: trackGeometry,
    );
    if (oldDisplay == Size.zero || newDisplay == Size.zero) return newLayout;

    var nextOffsetX = oldLayout.viewOffsetX;
    var nextOffsetY = oldLayout.viewOffsetY;
    if (oldDisplay.width.abs() > 1e-4 && newDisplay.width.abs() > 1e-4) {
      nextOffsetX *= newDisplay.width / oldDisplay.width;
    }
    if (oldDisplay.height.abs() > 1e-4 && newDisplay.height.abs() > 1e-4) {
      nextOffsetY *= newDisplay.height / oldDisplay.height;
    }
    return newLayout.copyWith(
      viewOffsetX: nextOffsetX,
      viewOffsetY: nextOffsetY,
    );
  }

  bool _rescaleViewOffsetForResize(
    int oldWidth,
    int oldHeight,
    int newWidth,
    int newHeight,
  ) {
    final current = layout();
    final trackGeometry = tracks()
        .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry.info))
        .toList();
    final oldDisplay = computeDisplayPixelSizeForLayout(
      viewportWidth: oldWidth,
      viewportHeight: oldHeight,
      layout: current,
      tracks: trackGeometry,
    );
    final newDisplay = computeDisplayPixelSizeForLayout(
      viewportWidth: newWidth,
      viewportHeight: newHeight,
      layout: current,
      tracks: trackGeometry,
    );
    if (oldDisplay == Size.zero || newDisplay == Size.zero) return false;

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
      return false;
    }
    _updateLayout(
      (layout) =>
          layout.copyWith(viewOffsetX: nextOffsetX, viewOffsetY: nextOffsetY),
    );
    return true;
  }
}
