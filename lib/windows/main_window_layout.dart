part of 'main_window.dart';

extension _MainWindowLayout on _MainWindowState {
  void _toggleLayoutMode() {
    _setLayoutMode(
      _layout.mode == LayoutMode.sideBySide
          ? LayoutMode.splitScreen
          : LayoutMode.sideBySide,
    );
  }

  void _setLayoutMode(int mode) {
    _updateLayout((layout) => layout.copyWith(mode: mode));
    _markLayoutDirty();
  }

  void _setZoom(double ratio) {
    _updateLayout(
      (layout) => layout.copyWith(
        zoomRatio: ratio.clamp(LayoutState.zoomMin, LayoutState.zoomMax),
      ),
    );
    _markLayoutDirty();
  }

  void _setSplitPos(double pos) {
    _updateLayout((layout) => layout.copyWith(splitPos: pos.clamp(0.0, 1.0)));
    _markLayoutDirty();
  }

  void _panByDelta(double dx, double dy) {
    _updateLayout(
      (layout) => layout.copyWith(
        viewOffsetX: layout.viewOffsetX + dx,
        viewOffsetY: layout.viewOffsetY + dy,
      ),
    );
    _markLayoutDirty();
  }

  void _onPan(Offset delta) {
    _panByDelta(delta.dx, delta.dy);
  }

  void _onSplit(double normalizedX) {
    _setSplitPos(normalizedX);
  }

  void _onZoom(double scrollDelta, Offset localPos) {
    final factor = scrollDelta > 0 ? 0.9 : 1.1;
    final newZoom = (_layout.zoomRatio * factor).clamp(
      LayoutState.zoomMin,
      LayoutState.zoomMax,
    );

    if (newZoom == LayoutState.zoomMin && factor < 1.0) {
      _updateLayout(
        (layout) =>
            layout.copyWith(zoomRatio: newZoom, viewOffsetX: 0, viewOffsetY: 0),
      );
      _markLayoutDirty();
      return;
    }

    final actualFactor = newZoom / _layout.zoomRatio;

    if (_viewportWidth <= 0 || _viewportHeight <= 0) {
      _updateLayout((layout) => layout.copyWith(zoomRatio: newZoom));
      _markLayoutDirty();
      return;
    }

    // Compute cursor position in slot-normalized coords and slot pixel size.
    double cursorX, cursorY, slotW, slotH;
    if (_layout.mode == LayoutMode.sideBySide) {
      final n = _trackManager.count > 0 ? _trackManager.count : 1;
      final nx = localPos.dx / _viewportWidth;
      final ny = localPos.dy / _viewportHeight;
      final slotIndex = (nx * n).floor().clamp(0, n - 1);
      cursorX = nx * n - slotIndex;
      cursorY = ny;
      slotW = _viewportWidth / n;
      slotH = _viewportHeight.toDouble();
    } else {
      cursorX = localPos.dx / _viewportWidth;
      cursorY = localPos.dy / _viewportHeight;
      slotW = _viewportWidth.toDouble();
      slotH = _viewportHeight.toDouble();
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
    _markLayoutDirty();
  }

  void _onPointerButton(bool panning, bool splitting) {
    // No-op for now; could show cursor changes etc.
  }

  void _onViewportResize(int width, int height) {
    if (width == _viewportWidth && height == _viewportHeight) return;
    _viewportWidth = width;
    _viewportHeight = height;
    _resizeDebounceTimer?.cancel();
    _resizeDebounceTimer = Timer(_MainWindowState._viewportResizeDebounce, () {
      if (!mounted) return;
      _markResizeDirty();
    });
  }

  void _markResizeDirty() {
    _resizeDirty = true;
    _layoutTicker?.start();
  }

  void _onZoomComboChanged(double value) {
    _updateLayout((layout) => layout.copyWith(zoomRatio: value));
    _markLayoutDirty();
  }

  void _markLayoutDirty() {
    _layoutDirty = true;
    _layoutTicker?.start();
  }

  void _startLayoutTicker() {
    _layoutTicker = createTicker((_) {
      unawaited(_flushPendingLayout());
    });
  }

  Future<void> _flushPendingLayout() async {
    if (_layoutFlushInProgress) return;
    if (_textureId == null) {
      _resizeDirty = false;
      _layoutDirty = false;
      _layoutTicker?.stop();
      return;
    }

    _layoutFlushInProgress = true;
    try {
      while (mounted && (_resizeDirty || _layoutDirty)) {
        if (_layoutDirty) {
          final layout = _layout;
          _layoutDirty = false;
          await _controller.applyLayout(layout);
          if (!mounted) return;
        }

        if (_resizeDirty && _viewportWidth > 0 && _viewportHeight > 0) {
          final width = _viewportWidth;
          final height = _viewportHeight;
          _resizeDirty = false;
          await _controller.resize(width, height);
          if (!mounted) return;
          if (!_layoutDirty) {
            final layout = await _controller.getLayout();
            if (!mounted) return;
            _replaceLayout(layout);
          }
        } else if (_resizeDirty) {
          _resizeDirty = false;
        }
      }
    } finally {
      _layoutFlushInProgress = false;
      if (mounted) {
        if (_resizeDirty || _layoutDirty) {
          _layoutTicker?.start();
        } else {
          _layoutTicker?.stop();
        }
      }
    }
  }
}
