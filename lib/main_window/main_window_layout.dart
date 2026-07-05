import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import '../app_log.dart';
import '../marks/quick_mark.dart';
import '../native_compositor_flags.dart';
import '../track_manager.dart';
import '../utils/async_guard.dart';
import '../video_renderer_controller.dart';
import '../viewport/display_geometry.dart';
import '../viewport/viewport_projection_diagnostics.dart';
import '../widgets/analysis_overlay_controls.dart';
import 'main_window_state.dart';

class MainWindowLayoutCoordinator {
  static const Duration viewportResizeDebounce = Duration(milliseconds: 80);
  static const double timelineTrackRowLogicalHeight = 40.0;
  static const Duration _debugInteractionSampleInterval = Duration(
    milliseconds: 250,
  );
  static const Duration _resizePacingLogInterval = Duration(milliseconds: 250);

  final TickerProvider vsync;
  final NativePlayerController controller;
  final MainWindowStateStore stateStore;
  final TrackManager trackManager;
  final bool Function() mounted;
  final bool Function() sourceProjectionEnabled;
  final void Function(int width, int height)? onNativeResizeCommitted;

  Ticker? _ticker;
  Timer? _resizeDebounceTimer;
  bool _layoutDirty = false;
  bool _resizeDirty = false;
  Future<void>? _activeFlush;
  bool _disposed = false;
  Future<void>? _preemptResizeFuture;
  int? _queuedPreemptWidth;
  int? _queuedPreemptHeight;
  bool _nativeCompositorTransformActive = false;
  String? _lastPrewarmedMarksSidebarTargetKey;
  DateTime? _lastDebugInteractionSampleAt;
  DateTime? _lastViewportResizePacingLogAt;
  DateTime? _lastNativeResizePacingLogAt;
  int _debugPanUpdates = 0;
  int _debugZoomUpdates = 0;
  int _debugSplitUpdates = 0;
  int _debugDeferredLayoutUpdates = 0;
  int _debugProjectionPublishes = 0;
  int _debugViewportResizeReports = 0;
  int _debugNativeResizeFlushes = 0;

  int viewportWidth = 0;
  int viewportHeight = 0;
  double viewportDevicePixelRatio = 1.0;

  MainWindowStateModel get _state => stateStore.value;

  int? textureId() => _state.textureId;
  LayoutState layout() => _state.layout;
  void setLayout(LayoutState layout) => stateStore.setLayout(layout);
  int trackCount() => trackManager.count;
  List<TrackEntry> tracks() => trackManager.entries;
  bool get _hasNativePlayer => controller.hasPlayer || textureId() != null;

  List<int> _orderForTrackInfos(List<TrackInfo> entries) {
    final result = List<int>.filled(4, -1);
    for (var i = 0; i < entries.length && i < result.length; i++) {
      result[i] = entries[i].fileId;
    }
    return result;
  }

  void _logDebugInteractionSample(
    String event, {
    Offset? delta,
    double? zoomFactor,
    double? splitPos,
    required bool transformed,
    required bool deferred,
  }) {
    final now = DateTime.now();
    final last = _lastDebugInteractionSampleAt;
    if (last != null &&
        now.difference(last) < _debugInteractionSampleInterval) {
      return;
    }
    _lastDebugInteractionSampleAt = now;
    final current = layout();
    log.fine(
      '[WindowsCompositorDebug] viewport layout sample '
      'event=$event pan=$_debugPanUpdates zoom=$_debugZoomUpdates '
      'split=$_debugSplitUpdates deferred=$_debugDeferredLayoutUpdates '
      'projection=$_debugProjectionPublishes transformed=$transformed '
      'deferredThis=$deferred nativeActive=${_state.nativeCompositorActive} '
      'sourceProjection=${sourceProjectionEnabled()} '
      'nativeTransform=$_nativeCompositorTransformActive '
      'layoutDirty=$_layoutDirty resizeDirty=$_resizeDirty '
      'activeFlush=${_activeFlush != null} viewport=${viewportWidth}x$viewportHeight '
      'tracks=${trackCount()} mode=${current.mode} '
      'zoomRatio=${current.zoomRatio.toStringAsFixed(3)} '
      'offset=(${current.viewOffsetX.toStringAsFixed(1)},'
      '${current.viewOffsetY.toStringAsFixed(1)}) '
      'delta=${_debugOffset(delta)} '
      'zoomFactor=${zoomFactor?.toStringAsFixed(4) ?? ""} '
      'splitPos=${splitPos?.toStringAsFixed(4) ?? ""}',
    );
  }

  String _debugOffset(Offset? value) {
    if (value == null) return '';
    return '(${value.dx.toStringAsFixed(1)},${value.dy.toStringAsFixed(1)})';
  }

  MainWindowLayoutCoordinator({
    required this.vsync,
    required this.controller,
    required this.stateStore,
    required this.trackManager,
    required this.mounted,
    this.onNativeResizeCommitted,
    bool Function()? sourceProjectionEnabled,
  }) : sourceProjectionEnabled =
           sourceProjectionEnabled ??
           (() => NativeCompositorFlags.sourceProjection) {
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
    _cancelNativeCompositorViewportTransform();
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
    _cancelNativeCompositorViewportTransform();
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
    _cancelNativeCompositorViewportTransform();
    _updateLayout(
      (layout) => layout.copyWith(
        zoomRatio: ratio.clamp(LayoutState.zoomMin, LayoutState.zoomMax),
      ),
    );
    markLayoutDirty();
  }

  void setSplitPos(double pos) {
    if (_disposed) return;
    final nextPos = pos.clamp(0.0, 1.0).toDouble();
    if (layout().splitPos == nextPos) return;
    _debugSplitUpdates++;
    _updateLayout((layout) => layout.copyWith(splitPos: nextPos));
    final transformed = _updateNativeCompositorPanTransform();
    markLayoutDirty();
    _logDebugInteractionSample(
      'split',
      splitPos: nextPos,
      transformed: transformed,
      deferred: false,
    );
  }

  void panByDelta(double dx, double dy) {
    if (_disposed) return;
    ViewportProjectionDiagnostics.instance.record('layoutPan');
    _debugPanUpdates++;
    final nextOffsetX = layout().viewOffsetX + dx;
    final nextOffsetY = layout().viewOffsetY + dy;
    _updateLayout(
      (layout) =>
          layout.copyWith(viewOffsetX: nextOffsetX, viewOffsetY: nextOffsetY),
    );
    final transformed = _updateNativeCompositorPanTransform();
    markLayoutDirty(deferNativeCompositorFlush: transformed);
    _logDebugInteractionSample(
      'pan',
      delta: Offset(dx, dy),
      transformed: transformed,
      deferred: transformed,
    );
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
    ViewportProjectionDiagnostics.instance.record('layoutZoom');
    _debugZoomUpdates++;
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
      final transformed = _updateNativeCompositorZoomTransform(
        actualFactor: newZoom / currentLayout.zoomRatio,
      );
      markLayoutDirty(deferNativeCompositorFlush: transformed);
      _logDebugInteractionSample(
        'zoom-min',
        zoomFactor: newZoom / currentLayout.zoomRatio,
        transformed: transformed,
        deferred: transformed,
      );
      return;
    }

    final actualFactor = newZoom / currentLayout.zoomRatio;

    if (viewportWidth <= 0 || viewportHeight <= 0) {
      _updateLayout((layout) => layout.copyWith(zoomRatio: newZoom));
      markLayoutDirty();
      _logDebugInteractionSample(
        'zoom-no-viewport',
        zoomFactor: actualFactor,
        transformed: false,
        deferred: false,
      );
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
    final transformed = _updateNativeCompositorZoomTransform(
      actualFactor: actualFactor,
    );
    markLayoutDirty(deferNativeCompositorFlush: transformed);
    _logDebugInteractionSample(
      'zoom',
      zoomFactor: actualFactor,
      transformed: transformed,
      deferred: transformed,
    );
  }

  void onPointerButton(bool panning, bool splitting) {
    if (_disposed) return;
    if (splitting) {
      _cancelNativeCompositorViewportTransform();
      return;
    }
    if (!panning) {
      _finishNativeCompositorViewportTransformInteraction();
    }
  }

  void onViewportResize(
    int width,
    int height,
    double devicePixelRatio, {
    bool immediate = false,
  }) {
    if (_disposed) return;
    _cancelNativeCompositorViewportTransform();
    if (devicePixelRatio > 0) {
      viewportDevicePixelRatio = devicePixelRatio;
    }
    if (width == viewportWidth && height == viewportHeight) return;
    final previousWidth = viewportWidth;
    final previousHeight = viewportHeight;
    log.fine(
      '[WindowsCompositorDebug] layout onViewportResize '
      '${previousWidth}x$previousHeight -> ${width}x$height '
      'dpr=${devicePixelRatio.toStringAsFixed(3)} '
      'immediate=$immediate layoutDirty=$_layoutDirty '
      'nativeActive=${_state.nativeCompositorActive} '
      'tracks=${trackCount()}',
    );
    if (!_layoutDirty && previousWidth > 0 && previousHeight > 0) {
      _rescaleViewOffsetForResize(previousWidth, previousHeight, width, height);
    }
    viewportWidth = width;
    viewportHeight = height;
    _prewarmNextMarksSidebarViewportTarget();
    _resizeDebounceTimer?.cancel();
    _debugViewportResizeReports++;
    final now = DateTime.now();
    final lastResizeLog = _lastViewportResizePacingLogAt;
    final shouldLogResize =
        _debugViewportResizeReports <= 8 ||
        lastResizeLog == null ||
        now.difference(lastResizeLog) >= _resizePacingLogInterval;
    if (shouldLogResize) {
      _lastViewportResizePacingLogAt = now;
      log.info(
        '[WindowsResizePacing] dart viewportResize '
        'count=$_debugViewportResizeReports '
        'previous=${previousWidth}x$previousHeight next=${width}x$height '
        'dpr=${devicePixelRatio.toStringAsFixed(3)} '
        'immediate=$immediate schedule=${immediate ? 'immediate' : 'debounce'} '
        'nativeActive=${_state.nativeCompositorActive} '
        'layoutDirty=$_layoutDirty resizeDirty=$_resizeDirty '
        'activeFlush=${_activeFlush != null}',
      );
    }
    if (immediate) {
      _resizeDebounceTimer = null;
      _markResizeDirty();
      return;
    }
    _resizeDebounceTimer = Timer(viewportResizeDebounce, () {
      if (_disposed || !mounted()) return;
      log.info(
        '[WindowsResizePacing] dart viewportResize debounceFire '
        'target=${viewportWidth}x$viewportHeight '
        'reports=$_debugViewportResizeReports '
        'layoutDirty=$_layoutDirty resizeDirty=$_resizeDirty '
        'activeFlush=${_activeFlush != null}',
      );
      _markResizeDirty();
    });
  }

  Future<void> preemptTimelineTrackCountChange({
    required int previousCount,
    required int nextCount,
  }) async {
    if (_disposed || !_hasNativePlayer) return;
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
    if (_disposed || !_hasNativePlayer) return;
    if (width <= 0 || height <= 0) return;
    if (width == viewportWidth && height == viewportHeight) return;

    final previousWidth = viewportWidth;
    final previousHeight = viewportHeight;
    log.fine(
      '[WindowsCompositorDebug] layout preemptViewportResize '
      '${previousWidth}x$previousHeight -> ${width}x$height '
      'layoutDirty=$_layoutDirty activeFlush=${_activeFlush != null} '
      'tracks=${trackCount()}',
    );
    _resizeDebounceTimer?.cancel();
    _resizeDebounceTimer = null;
    _resizeDirty = false;
    if (_layoutDirty) {
      final pendingLayout = layout();
      _layoutDirty = false;
      await _applyLayoutToNative(pendingLayout);
      if (_disposed || !mounted()) return;
    }
    if (previousWidth > 0 && previousHeight > 0) {
      _rescaleViewOffsetForResize(previousWidth, previousHeight, width, height);
    }
    viewportWidth = width;
    viewportHeight = height;
    await controller.resize(width, height);
    log.fine(
      '[WindowsCompositorDebug] layout preemptViewportResize native complete '
      '${width}x$height',
    );
    onNativeResizeCommitted?.call(width, height);
    if (_disposed || !mounted()) return;
    final nextLayout = await controller.getLayout();
    if (_disposed || !mounted()) return;
    setLayout(nextLayout);
    _prepareNativeCompositorSourceCache(nextLayout);
    _prewarmNextMarksSidebarViewportTarget();
  }

  void toggleMarksSidebar() {
    setMarksSidebarVisible(!_state.marksSidebarVisible);
  }

  void setMarksSidebarVisible(bool visible) {
    if (_state.marksSidebarVisible == visible) return;
    final viewportDelta = visible
        ? -_state.marksSidebarWidth
        : _state.marksSidebarWidth;
    if (visible) {
      requestPreemptViewportLogicalSizeDelta(widthDelta: viewportDelta);
      stateStore.setMarksSidebarVisible(true);
    } else {
      stateStore.setMarksSidebarVisible(false);
      SchedulerBinding.instance.addPostFrameCallback((_) {
        if (_disposed || !mounted() || _state.marksSidebarVisible) return;
        requestPreemptViewportLogicalSizeDelta(widthDelta: viewportDelta);
      });
    }
  }

  void setMarksSidebarWidth(double width) {
    final next = width
        .clamp(kMinMarksSidebarWidth, kMaxMarksSidebarWidth)
        .toDouble();
    if (next == _state.marksSidebarWidth) return;
    if (_state.marksSidebarVisible) {
      requestPreemptViewportLogicalSizeDelta(
        widthDelta: _state.marksSidebarWidth - next,
      );
    }
    stateStore.setMarksSidebarWidth(next);
    _lastPrewarmedMarksSidebarTargetKey = null;
    if (!_state.marksSidebarVisible) {
      _prewarmNextMarksSidebarViewportTarget();
    }
  }

  void _prewarmNextMarksSidebarViewportTarget() {
    if (_disposed || textureId() == null) return;
    if (viewportWidth <= 0 || viewportHeight <= 0) return;
    final dpr = viewportDevicePixelRatio > 0 ? viewportDevicePixelRatio : 1.0;
    final widthDelta = _state.marksSidebarVisible
        ? _state.marksSidebarWidth
        : -_state.marksSidebarWidth;
    final targetWidth = (viewportWidth + widthDelta * dpr)
        .round()
        .clamp(1, 1 << 30)
        .toInt();
    final targetHeight = viewportHeight;
    if (targetWidth == viewportWidth && targetHeight == viewportHeight) return;
    final key =
        '$targetWidth:$targetHeight:${_state.marksSidebarWidth}:'
        '${_state.marksSidebarVisible}:$dpr';
    if (_lastPrewarmedMarksSidebarTargetKey == key) return;
    _lastPrewarmedMarksSidebarTargetKey = key;
    fireAndLog(
      'prewarm marks sidebar viewport target',
      controller.prewarmNativePresentationTargetSize(targetWidth, targetHeight),
    );
  }

  void setAnalysisOverlayControlsVisible(bool visible) {
    if (_state.analysisOverlayControlsVisible == visible) return;
    if (!_state.fullScreen) {
      final viewportDelta = visible
          ? -AnalysisOverlayStrip.height
          : AnalysisOverlayStrip.height;
      requestPreemptViewportLogicalSizeDelta(heightDelta: viewportDelta);
    }
    stateStore.setAnalysisOverlayControlsVisible(visible);
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
    log.fine(
      '[WindowsCompositorDebug] layout queuePreemptResize '
      'base=${baseWidth}x$baseHeight '
      'delta=(${widthDelta.toStringAsFixed(1)},'
      '${heightDelta.toStringAsFixed(1)}) '
      'dpr=${dpr.toStringAsFixed(3)} next=${nextWidth}x$nextHeight',
    );
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
    _cancelNativeCompositorViewportTransform();
    markLayoutDirty();
  }

  void markLayoutDirty({bool deferNativeCompositorFlush = false}) {
    if (_disposed) return;
    _layoutDirty = true;
    if (deferNativeCompositorFlush) {
      _debugDeferredLayoutUpdates++;
      // The compositor already has the full current projection; native renderer
      // catches up once at pointer-up or a playback transition.
      return;
    }
    _startTicker();
  }

  /// Flushes deferred interaction state around playback transitions
  /// (play/pause/step). The deferred layout reaches native before playback
  /// resumes. The native compositor source projection is a full-layout path now,
  /// so it stays live across authoritative layout flushes.
  Future<void> onPlaybackStateChanged({required bool playing}) async {
    if (_disposed) return;
    final active = _activeFlush;
    if (active != null) {
      await active;
    }
    if (_disposed || !mounted()) return;
    if (!_layoutDirty) return;
    _layoutDirty = true;
    await flushPendingLayout();
  }

  void _markResizeDirty() {
    if (_disposed) return;
    _resizeDirty = true;
    log.fine(
      '[WindowsCompositorDebug] layout resizeDirty '
      '${viewportWidth}x$viewportHeight '
      'layoutDirty=$_layoutDirty activeFlush=${_activeFlush != null}',
    );
    _startTicker();
  }

  void _startTicker() {
    final ticker = _ticker;
    if (ticker == null || ticker.isActive) return;
    ticker.start();
  }

  Future<void> flushPendingLayout() {
    final active = _activeFlush;
    if (active != null) return active;
    late final Future<void> flush;
    flush = _flushPendingLayoutLoop().whenComplete(() {
      if (identical(_activeFlush, flush)) {
        _activeFlush = null;
      }
    });
    _activeFlush = flush;
    return flush;
  }

  Future<void> _flushPendingLayoutLoop() async {
    if (_disposed) return;
    if (textureId() == null) {
      _resizeDirty = false;
      _layoutDirty = false;
      _ticker?.stop();
      return;
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
            await _applyLayoutToNative(pendingLayout);
            if (_disposed || !mounted()) return;
          }
          await controller.resize(width, height);
          _debugNativeResizeFlushes++;
          final now = DateTime.now();
          final lastNativeLog = _lastNativeResizePacingLogAt;
          final shouldLogNative =
              _debugNativeResizeFlushes <= 8 ||
              lastNativeLog == null ||
              now.difference(lastNativeLog) >= _resizePacingLogInterval;
          if (shouldLogNative) {
            _lastNativeResizePacingLogAt = now;
            log.info(
              '[WindowsResizePacing] dart nativeResizeFlush '
              'count=$_debugNativeResizeFlushes '
              'target=${width}x$height '
              'layoutDirty=$_layoutDirty resizeDirty=$_resizeDirty '
              'activeFlush=${_activeFlush != null}',
            );
          }
          log.fine(
            '[WindowsCompositorDebug] layout flush native resize complete '
            '${width}x$height layoutDirty=$_layoutDirty',
          );
          onNativeResizeCommitted?.call(width, height);
          if (_disposed || !mounted()) return;
          final nextLayout = await controller.getLayout();
          if (_disposed || !mounted()) return;
          setLayout(nextLayout);
          _prepareNativeCompositorSourceCache(nextLayout);
        } else if (_resizeDirty) {
          _resizeDirty = false;
        }

        if (_layoutDirty) {
          final nextLayout = layout();
          _layoutDirty = false;
          await _applyLayoutToNative(nextLayout);
          if (_disposed || !mounted()) return;
        }
      }
    } finally {
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

  bool get _canUseNativeCompositorViewportTransform {
    return sourceProjectionEnabled() &&
        _state.nativeCompositorActive &&
        _hasNativePlayer &&
        viewportWidth > 0 &&
        viewportHeight > 0 &&
        trackCount() > 0 &&
        (layout().mode == LayoutMode.sideBySide ||
            layout().mode == LayoutMode.splitScreen);
  }

  bool _canPrepareNativeCompositorSourceProjection({
    required int candidateTrackCount,
    required int candidateViewportWidth,
    required int candidateViewportHeight,
    required LayoutState candidateLayout,
  }) {
    return sourceProjectionEnabled() &&
        _state.nativeCompositorActive &&
        _hasNativePlayer &&
        candidateViewportWidth > 0 &&
        candidateViewportHeight > 0 &&
        candidateTrackCount > 0 &&
        (candidateLayout.mode == LayoutMode.sideBySide ||
            candidateLayout.mode == LayoutMode.splitScreen);
  }

  void _ensureNativeCompositorViewportTransform() {
    if (_nativeCompositorTransformActive) return;
    _nativeCompositorTransformActive = true;
    // The first projection publish subscribes the source cache. Paused keeps a
    // frozen bake; playing refreshes the ring from native frame callbacks.
    // This flag is now only an interaction marker; the compositor no longer has
    // a residual transform layer that needs to be cleared at pointer-up.
  }

  bool _updateNativeCompositorPanTransform() {
    if (!_canUseNativeCompositorViewportTransform) {
      _cancelNativeCompositorViewportTransform();
      return false;
    }
    _ensureNativeCompositorViewportTransform();
    _publishNativeCompositorViewportTransform();
    return true;
  }

  bool _updateNativeCompositorZoomTransform({required double actualFactor}) {
    if (!_canUseNativeCompositorViewportTransform ||
        actualFactor <= 0 ||
        !actualFactor.isFinite ||
        actualFactor == 1.0) {
      if (!_canUseNativeCompositorViewportTransform) {
        _cancelNativeCompositorViewportTransform();
      }
      return false;
    }
    _ensureNativeCompositorViewportTransform();
    _publishNativeCompositorViewportTransform();
    return true;
  }

  Future<bool> preparePendingTrackAddVisualTopology(TrackInfo pendingTrack) {
    final current = tracks().map((entry) => entry.info).toList(growable: true);
    current.add(pendingTrack);
    final pendingLayout = layout().copyWith(
      order: _orderForTrackInfos(current),
    );
    final pendingHeight = _viewportHeightAfterTimelineTrackCountChange(
      previousCount: trackCount(),
      nextCount: current.length,
    );
    return _prepareNativeCompositorSourceCacheForTracks(
      current,
      pendingLayout,
      viewportWidthOverride: viewportWidth,
      viewportHeightOverride: pendingHeight,
    );
  }

  int _viewportHeightAfterTimelineTrackCountChange({
    required int previousCount,
    required int nextCount,
  }) {
    if (previousCount <= 0 ||
        nextCount <= 0 ||
        previousCount == nextCount ||
        viewportHeight <= 0) {
      return viewportHeight;
    }
    final rowDelta = nextCount - previousCount;
    final heightDelta =
        (rowDelta * timelineTrackRowLogicalHeight * viewportDevicePixelRatio)
            .round();
    if (heightDelta == 0) return viewportHeight;
    return (viewportHeight - heightDelta).clamp(1, 1 << 30).toInt();
  }

  void _prepareNativeCompositorSourceCache(LayoutState baseLayout) {
    fireAndLog(
      'prepare native compositor source cache',
      _prepareNativeCompositorSourceCacheForTracks(
        tracks().map((entry) => entry.info).toList(growable: false),
        baseLayout,
      ),
    );
  }

  Future<bool> _prepareNativeCompositorSourceCacheForTracks(
    List<TrackInfo> trackInfos,
    LayoutState baseLayout, {
    int? viewportWidthOverride,
    int? viewportHeightOverride,
  }) async {
    final candidateViewportWidth = viewportWidthOverride ?? viewportWidth;
    final candidateViewportHeight = viewportHeightOverride ?? viewportHeight;
    if (!_canPrepareNativeCompositorSourceProjection(
      candidateTrackCount: trackInfos.length,
      candidateViewportWidth: candidateViewportWidth,
      candidateViewportHeight: candidateViewportHeight,
      candidateLayout: baseLayout,
    )) {
      ViewportProjectionDiagnostics.instance.record(
        'projectionPrepareSkippedIneligible',
      );
      return false;
    }
    if (trackInfos.isEmpty) return false;
    ViewportProjectionDiagnostics.instance.record('projectionPrepare');
    final trackGeometry = trackInfos
        .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry))
        .toList();
    final projection = computeViewportLayoutProjection(
      viewportWidth: candidateViewportWidth,
      viewportHeight: candidateViewportHeight,
      layout: baseLayout,
      tracks: trackGeometry,
    );
    final sourceSlots = <int>[];
    final sourceOrder = List<int>.filled(4, 0);
    final displayOffsetX = List<double>.filled(4, 0.0);
    final displayOffsetY = List<double>.filled(4, 0.0);
    final invDisplaySizeX = List<double>.filled(4, 0.0);
    final invDisplaySizeY = List<double>.filled(4, 0.0);
    final viewOffsetUvX = List<double>.filled(4, 0.0);
    final viewOffsetUvY = List<double>.filled(4, 0.0);
    final entriesByFileId = {
      for (final entry in trackInfos) entry.fileId: entry,
    };

    for (final entry in trackInfos) {
      final slot = entry.slot;
      if (slot < 0 || slot >= 4) continue;
      final trackProjection = projection.projectionForFileId(entry.fileId);
      if (trackProjection == null) continue;
      sourceSlots.add(slot);
      displayOffsetX[slot] = trackProjection.displayOffsetX;
      displayOffsetY[slot] = trackProjection.displayOffsetY;
      invDisplaySizeX[slot] = trackProjection.invDisplaySizeX;
      invDisplaySizeY[slot] = trackProjection.invDisplaySizeY;
      viewOffsetUvX[slot] = trackProjection.viewOffsetUvX;
      viewOffsetUvY[slot] = trackProjection.viewOffsetUvY;
    }
    for (
      var index = 0;
      index < projection.orderedTracks.length && index < 4;
      index++
    ) {
      final entry = entriesByFileId[projection.orderedTracks[index].fileId];
      if (entry != null && entry.slot >= 0 && entry.slot < 4) {
        sourceOrder[index] = entry.slot;
      }
    }
    if (sourceSlots.isEmpty) return false;
    await controller.prepareNativeCompositorSourceCache(
      sourceSlots: sourceSlots,
      sourceOrder: sourceOrder,
      mode: baseLayout.mode,
      splitPos: baseLayout.splitPos,
      activeTrackCount: trackInfos.length,
      displayOffsetX: displayOffsetX,
      displayOffsetY: displayOffsetY,
      invDisplaySizeX: invDisplaySizeX,
      invDisplaySizeY: invDisplaySizeY,
      viewOffsetUvX: viewOffsetUvX,
      viewOffsetUvY: viewOffsetUvY,
    );
    return true;
  }

  void refreshNativeCompositorOverlay() {
    if (_disposed) return;
    _prepareNativeCompositorSourceCache(layout());
  }

  void onNativeCompositorAvailabilityChanged({required bool active}) {
    if (_disposed || active) return;
    _cancelNativeCompositorViewportTransform();
    fireAndLog(
      'clear inactive native compositor source cache',
      controller.clearNativeCompositorSourceCache(
        reason: 'native compositor inactive',
      ),
    );
  }

  void onTrackSetChanged() {
    if (_disposed) return;
    if (trackCount() == 0) {
      _cancelNativeCompositorViewportTransform();
      if (_state.nativeCompositorActive ||
          NativeCompositorFlags.sourceProjection) {
        fireAndLog(
          'clear zero-track native compositor source cache',
          controller.clearNativeCompositorSourceCache(reason: 'zero tracks'),
        );
      }
      return;
    }
    _prepareNativeCompositorSourceCache(layout());
  }

  void _publishNativeCompositorViewportTransform() {
    if (!_nativeCompositorTransformActive || _disposed) return;
    _debugProjectionPublishes++;
    ViewportProjectionDiagnostics.instance.record('projectionPublishAttempt');
    _prepareNativeCompositorSourceCache(layout());
  }

  void _finishNativeCompositorViewportTransformInteraction() {
    if (_nativeCompositorTransformActive && _layoutDirty) {
      _startTicker();
    }
  }

  void _cancelNativeCompositorViewportTransform() {
    if (!_nativeCompositorTransformActive) return;
    _nativeCompositorTransformActive = false;
  }

  Future<void> _applyLayoutToNative(LayoutState nextLayout) async {
    final hadTransform = _nativeCompositorTransformActive;
    await controller.applyLayout(nextLayout);
    _prepareNativeCompositorSourceCache(nextLayout);
    if (!hadTransform || _disposed || !_nativeCompositorTransformActive) {
      return;
    }
    _resetNativeCompositorViewportTransformAfterAuthoritativeLayout();
  }

  void _resetNativeCompositorViewportTransformAfterAuthoritativeLayout() {
    if (!_nativeCompositorTransformActive) return;
    _nativeCompositorTransformActive = false;
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
