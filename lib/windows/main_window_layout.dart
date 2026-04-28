import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import '../video_renderer_controller.dart';

class MainWindowLayoutCoordinator {
  static const Duration viewportResizeDebounce = Duration(milliseconds: 80);

  final TickerProvider vsync;
  final VideoRendererController controller;
  final bool Function() mounted;
  final int? Function() textureId;
  final LayoutState Function() layout;
  final void Function(LayoutState layout) setLayout;
  final int Function() trackCount;

  Ticker? _ticker;
  Timer? _resizeDebounceTimer;
  bool _layoutDirty = false;
  bool _resizeDirty = false;
  bool _flushInProgress = false;

  int viewportWidth = 0;
  int viewportHeight = 0;

  MainWindowLayoutCoordinator({
    required this.vsync,
    required this.controller,
    required this.mounted,
    required this.textureId,
    required this.layout,
    required this.setLayout,
    required this.trackCount,
  }) {
    _ticker = vsync.createTicker((_) {
      unawaited(flushPendingLayout());
    });
  }

  void dispose() {
    _resizeDebounceTimer?.cancel();
    _ticker?.dispose();
  }

  void toggleLayoutMode() {
    setLayoutMode(
      layout().mode == LayoutMode.sideBySide
          ? LayoutMode.splitScreen
          : LayoutMode.sideBySide,
    );
  }

  void setLayoutMode(int mode) {
    _updateLayout((layout) => layout.copyWith(mode: mode));
    markLayoutDirty();
  }

  void setZoom(double ratio) {
    _updateLayout(
      (layout) => layout.copyWith(
        zoomRatio: ratio.clamp(LayoutState.zoomMin, LayoutState.zoomMax),
      ),
    );
    markLayoutDirty();
  }

  void setSplitPos(double pos) {
    _updateLayout((layout) => layout.copyWith(splitPos: pos.clamp(0.0, 1.0)));
    markLayoutDirty();
  }

  void panByDelta(double dx, double dy) {
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

  void onViewportResize(int width, int height) {
    if (width == viewportWidth && height == viewportHeight) return;
    viewportWidth = width;
    viewportHeight = height;
    _resizeDebounceTimer?.cancel();
    _resizeDebounceTimer = Timer(viewportResizeDebounce, () {
      if (!mounted()) return;
      _markResizeDirty();
    });
  }

  void onZoomComboChanged(double value) {
    _updateLayout((layout) => layout.copyWith(zoomRatio: value));
    markLayoutDirty();
  }

  void markLayoutDirty() {
    _layoutDirty = true;
    _ticker?.start();
  }

  void _markResizeDirty() {
    _resizeDirty = true;
    _ticker?.start();
  }

  Future<void> flushPendingLayout() async {
    if (_flushInProgress) return;
    if (textureId() == null) {
      _resizeDirty = false;
      _layoutDirty = false;
      _ticker?.stop();
      return;
    }

    _flushInProgress = true;
    try {
      while (mounted() && (_resizeDirty || _layoutDirty)) {
        if (_layoutDirty) {
          final nextLayout = layout();
          _layoutDirty = false;
          await controller.applyLayout(nextLayout);
          if (!mounted()) return;
        }

        if (_resizeDirty && viewportWidth > 0 && viewportHeight > 0) {
          final width = viewportWidth;
          final height = viewportHeight;
          _resizeDirty = false;
          await controller.resize(width, height);
          if (!mounted()) return;
          if (!_layoutDirty) {
            final nextLayout = await controller.getLayout();
            if (!mounted()) return;
            setLayout(nextLayout);
          }
        } else if (_resizeDirty) {
          _resizeDirty = false;
        }
      }
    } finally {
      _flushInProgress = false;
      if (mounted()) {
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
}
