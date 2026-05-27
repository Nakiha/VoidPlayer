import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import '../../app_log.dart';
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
  final bool _viewportTraceEnabled =
      Platform.environment['VOIDPLAYER_VIEWPORT_TRACE'] == '1';
  int _panTraceSeq = 0;
  int _flushTraceSeq = 0;
  Offset _panTraceAccumulatedDelta = Offset.zero;
  DateTime? _lastPanTraceAt;

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
    if (_viewportTraceEnabled) {
      _logViewportPanTrace(
        dx: dx,
        dy: dy,
        before: current,
        nextOffsetX: nextOffsetX,
        nextOffsetY: nextOffsetY,
      );
    }
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

    if (_viewportTraceEnabled) {
      log.info(
        '[ViewportTrace] zoom factor=${factor.toStringAsFixed(4)} '
        'actualFactor=${actualFactor.toStringAsFixed(4)} '
        'local=(${localPos.dx.toStringAsFixed(1)},${localPos.dy.toStringAsFixed(1)}) '
        'zoom=${currentLayout.zoomRatio.toStringAsFixed(4)}->${newZoom.toStringAsFixed(4)} '
        'offset=(${currentLayout.viewOffsetX.toStringAsFixed(1)},${currentLayout.viewOffsetY.toStringAsFixed(1)})'
        '->(${nextOffsetX.toStringAsFixed(1)},${nextOffsetY.toStringAsFixed(1)}) '
        'viewport=${viewportWidth}x$viewportHeight mode=${currentLayout.mode} '
        'layoutDirty=$_layoutDirty resizeDirty=$_resizeDirty flush=$_flushInProgress',
      );
    }

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

  void onZoomComboChanged(double value) {
    setZoom(value);
  }

  void markLayoutDirty() {
    if (_disposed) return;
    _layoutDirty = true;
    if (_viewportTraceEnabled) {
      final current = layout();
      log.info(
        '[ViewportTrace] mark-layout-dirty '
        'zoom=${current.zoomRatio.toStringAsFixed(4)} '
        'offset=(${current.viewOffsetX.toStringAsFixed(1)},${current.viewOffsetY.toStringAsFixed(1)}) '
        'resizeDirty=$_resizeDirty flush=$_flushInProgress',
      );
    }
    _startTicker();
  }

  void _markResizeDirty() {
    if (_disposed) return;
    _resizeDirty = true;
    if (_viewportTraceEnabled) {
      log.info(
        '[ViewportTrace] mark-resize-dirty viewport=${viewportWidth}x$viewportHeight '
        'layoutDirty=$_layoutDirty flush=$_flushInProgress',
      );
    }
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
    final flushSeq = ++_flushTraceSeq;
    if (_viewportTraceEnabled) {
      log.info(
        '[ViewportTrace] flush#$flushSeq begin '
        'layoutDirty=$_layoutDirty resizeDirty=$_resizeDirty '
        'viewport=${viewportWidth}x$viewportHeight',
      );
    }
    try {
      while (!_disposed && mounted() && (_resizeDirty || _layoutDirty)) {
        if (_resizeDirty && viewportWidth > 0 && viewportHeight > 0) {
          final width = viewportWidth;
          final height = viewportHeight;
          _resizeDirty = false;
          if (_layoutDirty) {
            final pendingLayout = layout();
            _layoutDirty = false;
            if (_viewportTraceEnabled) {
              _logViewportApplyTrace(
                flushSeq,
                'pre-resize-apply-layout',
                pendingLayout,
              );
            }
            await controller.applyLayout(pendingLayout);
            if (_disposed || !mounted()) return;
          }
          if (_viewportTraceEnabled) {
            log.info('[ViewportTrace] flush#$flushSeq resize ${width}x$height');
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
          if (_viewportTraceEnabled) {
            _logViewportApplyTrace(flushSeq, 'apply-layout', nextLayout);
          }
          await controller.applyLayout(nextLayout);
          if (_disposed || !mounted()) return;
        }
      }
    } finally {
      _flushInProgress = false;
      if (_viewportTraceEnabled) {
        log.info(
          '[ViewportTrace] flush#$flushSeq end '
          'layoutDirty=$_layoutDirty resizeDirty=$_resizeDirty',
        );
      }
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

  void _logViewportPanTrace({
    required double dx,
    required double dy,
    required LayoutState before,
    required double nextOffsetX,
    required double nextOffsetY,
  }) {
    final now = DateTime.now();
    final last = _lastPanTraceAt;
    _lastPanTraceAt = now;
    final dtMs = last == null ? -1 : now.difference(last).inMicroseconds / 1000;
    _panTraceAccumulatedDelta += Offset(dx, dy);
    log.info(
      '[ViewportTrace] pan#${++_panTraceSeq} '
      'dtMs=${dtMs < 0 ? "first" : dtMs.toStringAsFixed(2)} '
      'delta=(${dx.toStringAsFixed(1)},${dy.toStringAsFixed(1)}) '
      'accum=(${_panTraceAccumulatedDelta.dx.toStringAsFixed(1)},${_panTraceAccumulatedDelta.dy.toStringAsFixed(1)}) '
      'zoom=${before.zoomRatio.toStringAsFixed(4)} '
      'offset=(${before.viewOffsetX.toStringAsFixed(1)},${before.viewOffsetY.toStringAsFixed(1)})'
      '->(${nextOffsetX.toStringAsFixed(1)},${nextOffsetY.toStringAsFixed(1)}) '
      'viewport=${viewportWidth}x$viewportHeight mode=${before.mode} '
      'layoutDirty=$_layoutDirty resizeDirty=$_resizeDirty flush=$_flushInProgress',
    );
  }

  void _logViewportApplyTrace(
    int flushSeq,
    String phase,
    LayoutState nextLayout,
  ) {
    log.info(
      '[ViewportTrace] flush#$flushSeq $phase '
      'zoom=${nextLayout.zoomRatio.toStringAsFixed(4)} '
      'offset=(${nextLayout.viewOffsetX.toStringAsFixed(1)},${nextLayout.viewOffsetY.toStringAsFixed(1)}) '
      'viewport=${viewportWidth}x$viewportHeight mode=${nextLayout.mode} '
      'resizeDirty=$_resizeDirty',
    );
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
