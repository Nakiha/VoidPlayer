import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import '../app_log.dart';
import '../l10n/app_localizations.dart';
import '../marks/quick_mark.dart';
import '../platform/pointer_button_state_provider.dart';
import '../utils/pointer_gesture_utils.dart';
import '../video_renderer_controller.dart';
import '../viewport/display_geometry.dart';
import '../viewport/viewport_display_state.dart';
import '../viewport/viewport_interaction.dart';
import '../viewport/viewport_projection_diagnostics.dart';
import 'axtree_region.dart';
import 'quick_mark_overlay.dart';

class ViewportPanel extends StatefulWidget {
  final int? textureId;
  final ViewportDisplayState viewportState;
  final String? errorText;
  final LayoutState layout;

  final void Function(Offset delta) onPan;
  final void Function(double normalizedX) onSplit;
  final void Function(double factor, Offset localPosition) onZoom;
  final void Function(bool panning, bool splitting) onPointerButton;
  final void Function(int width, int height, double devicePixelRatio)? onResize;
  final void Function(
    int left,
    int top,
    int width,
    int height,
    int surfaceWidth,
    int surfaceHeight,
  )?
  onNativeCompositorViewportRect;
  final PointerButtonStateProvider pointerButtonStateProvider;
  final bool nativePlaybackAvailable;
  final ViewportInteractionPolicy interactionPolicy;
  final List<DisplayTrackGeometry> trackGeometry;
  final List<QuickMark> quickMarks;
  final QuickMark? quickMarkDraft;
  final int? selectedQuickMarkId;
  final bool nativeCompositorHole;
  final ValueChanged<Offset>? onQuickMarkStart;
  final ValueChanged<Offset>? onQuickMarkUpdate;
  final VoidCallback? onQuickMarkInteraction;
  final VoidCallback? onQuickMarkEnd;
  final VoidCallback? onQuickMarkCancel;
  final ValueChanged<int?>? onQuickMarkSelect;
  final ValueChanged<QuickMark>? onQuickMarkChanged;
  final ValueChanged<int>? onQuickMarkDeleted;
  final ValueChanged<int>? onQuickMarkFocus;

  const ViewportPanel({
    super.key,
    required this.textureId,
    required this.viewportState,
    this.errorText,
    required this.layout,
    required this.onPan,
    required this.onSplit,
    required this.onZoom,
    required this.onPointerButton,
    this.onResize,
    this.onNativeCompositorViewportRect,
    this.pointerButtonStateProvider = emptyPointerButtonStateProvider,
    this.nativePlaybackAvailable = true,
    this.interactionPolicy = defaultViewportInteractionPolicy,
    this.trackGeometry = const [],
    this.quickMarks = const [],
    this.quickMarkDraft,
    this.selectedQuickMarkId,
    this.nativeCompositorHole = false,
    this.onQuickMarkStart,
    this.onQuickMarkUpdate,
    this.onQuickMarkInteraction,
    this.onQuickMarkEnd,
    this.onQuickMarkCancel,
    this.onQuickMarkSelect,
    this.onQuickMarkChanged,
    this.onQuickMarkDeleted,
    this.onQuickMarkFocus,
  });

  @override
  State<ViewportPanel> createState() => _ViewportPanelState();
}

class _ViewportPanelState extends State<ViewportPanel> {
  static const double _splitHandlePhysicalWidth = 4.0;
  static const double _splitHandleTouchWidth = 28.0;
  static const double _splitHandleVisualWidth = 24.0;
  static const double _splitHandleVisualHeight = 38.0;
  static const double _wheelScrollDeltaPerStep = 120.0;
  static const double _wheelZoomFactorPerStep = 1.1;
  static const double _quickMarkBorderHitWidth = 12.0;
  static const double _quickMarkHandleHitSize = 18.0;
  static const double _quickMarkPanelWidth = 428.0;
  static const double _quickMarkPanelHeight = 36.0;
  static const double _quickMarkPanelGap = 10.0;
  static const Duration _debugInteractionSampleInterval = Duration(
    milliseconds: 250,
  );
  static const Duration _resizePacingLogInterval = Duration(milliseconds: 250);

  bool _panning = false;
  bool _splitting = false;
  bool _splitHandleDragging = false;
  bool _quickMarkDragging = false;
  bool _panZoomScaling = false;
  Offset _lastMouseLocalPos = Offset.zero;
  Size _lastReportedLogicalSize = Size.zero;
  Size _lastReportedCompositorLogicalSize = Size.zero;
  Offset _lastReportedGlobalOffset = Offset.infinite;
  Size _lastReportedSurfaceSize = Size.zero;
  double _lastReportedDevicePixelRatio = 0.0;
  double _lastPanZoomScale = 1.0;
  DateTime? _lastDebugInteractionSampleAt;
  DateTime? _lastResizePacingLogAt;
  DateTime? _lastViewportRectPacingLogAt;
  int _debugPointerMoveCount = 0;
  int _debugPointerHoverCount = 0;
  int _debugResizeReportCount = 0;
  int _debugViewportRectReportCount = 0;

  void _logDebugInteractionSample(
    String stage,
    PointerEvent event, {
    Offset? logicalDelta,
    Offset? physicalDelta,
    double? scaleDelta,
  }) {
    final now = DateTime.now();
    final last = _lastDebugInteractionSampleAt;
    if (last != null &&
        now.difference(last) < _debugInteractionSampleInterval) {
      return;
    }
    _lastDebugInteractionSampleAt = now;
    final message =
        '[WindowsCompositorDebug] viewport interaction sample '
        'stage=$stage moves=$_debugPointerMoveCount '
        'hovers=$_debugPointerHoverCount buttons=${event.buttons} '
        'local=(${event.localPosition.dx.toStringAsFixed(1)},'
        '${event.localPosition.dy.toStringAsFixed(1)}) '
        'panning=$_panning splitting=$_splitting '
        'quickMark=$_quickMarkDragging splitHandle=$_splitHandleDragging '
        'nativeHole=${widget.nativeCompositorHole} '
        'mode=${widget.layout.mode} '
        'zoom=${widget.layout.zoomRatio.toStringAsFixed(3)} '
        'offset=(${widget.layout.viewOffsetX.toStringAsFixed(1)},'
        '${widget.layout.viewOffsetY.toStringAsFixed(1)}) '
        'logicalDelta=${_debugOffset(logicalDelta)} '
        'physicalDelta=${_debugOffset(physicalDelta)} '
        'scaleDelta=${scaleDelta?.toStringAsFixed(4) ?? ""}';
    log.fine(message);
  }

  String _debugOffset(Offset? value) {
    if (value == null) return '';
    return '(${value.dx.toStringAsFixed(1)},${value.dy.toStringAsFixed(1)})';
  }

  void _syncDragButtons(
    int buttons,
    Offset localPosition, {
    bool allowWin32Recovery = false,
  }) {
    if (_splitHandleDragging || _quickMarkDragging) return;

    var dragIntent = widget.interactionPolicy.dragIntentForButtons(buttons);
    var wantsPan = dragIntent == ViewportDragIntent.pan;
    const wantsSplit = false;
    if (!wantsPan && !wantsSplit && allowWin32Recovery && buttons == 0) {
      dragIntent = widget.pointerButtonStateProvider.isSecondaryButtonDown
          ? ViewportDragIntent.pan
          : ViewportDragIntent.none;
      wantsPan = dragIntent == ViewportDragIntent.pan;
    }

    if (!wantsPan && !wantsSplit) {
      if (_panning || _splitting) {
        _panning = false;
        _splitting = false;
        widget.onPointerButton(false, false);
      }
      return;
    }

    if (wantsPan != _panning || wantsSplit != _splitting) {
      _panning = wantsPan;
      _splitting = wantsSplit;
      _lastMouseLocalPos = localPosition;
      widget.onPointerButton(_panning, _splitting);
    }
  }

  void _updateSplitFromLocalX(BuildContext context, double localX) {
    if (!_splitting || widget.layout.mode != LayoutMode.splitScreen) return;
    final box = context.findRenderObject() as RenderBox;
    if (box.size.width <= 0) return;
    widget.onSplit(localX / box.size.width);
  }

  double? _viewportLocalXFromGlobal(
    BuildContext context,
    Offset globalPosition,
  ) {
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) return null;
    return renderObject.globalToLocal(globalPosition).dx;
  }

  bool _isOnSplitHandle(BuildContext context, Offset localPosition) {
    if (widget.layout.mode != LayoutMode.splitScreen) return false;
    final box = context.findRenderObject() as RenderBox;
    if (box.size.width <= 0) return false;
    final handleX = box.size.width * widget.layout.splitPos;
    return (localPosition.dx - handleX).abs() <= _splitHandleTouchWidth / 2;
  }

  bool _isOnQuickMarkEditor(
    BuildContext context,
    Offset localPosition,
    double devicePixelRatio,
  ) {
    if (widget.quickMarks.isEmpty ||
        widget.trackGeometry.isEmpty ||
        devicePixelRatio <= 0) {
      return false;
    }
    final box = context.findRenderObject() as RenderBox;
    if (!box.hasSize || box.size.width <= 0 || box.size.height <= 0) {
      return false;
    }
    final projection = computeViewportLayoutProjection(
      viewportWidth: (box.size.width * devicePixelRatio).round(),
      viewportHeight: (box.size.height * devicePixelRatio).round(),
      layout: widget.layout,
      tracks: widget.trackGeometry,
    );
    final selectedMark = _selectedQuickMark;
    final selectedRect = selectedMark == null
        ? null
        : _quickMarkLogicalRect(projection, selectedMark, devicePixelRatio);
    if (selectedMark != null &&
        selectedRect != null &&
        _quickMarkPanelRect(selectedRect, box.size).contains(localPosition)) {
      return true;
    }
    if (selectedMark != null &&
        selectedRect != null &&
        _isOnSelectedQuickMarkHandle(
          selectedMark,
          selectedRect,
          localPosition,
        )) {
      return true;
    }
    for (final mark in widget.quickMarks) {
      if (_isOnQuickMarkInteractiveArea(
        projection,
        mark,
        localPosition,
        devicePixelRatio,
      )) {
        return true;
      }
    }
    return false;
  }

  QuickMark? get _selectedQuickMark {
    final selectedId = widget.selectedQuickMarkId;
    if (selectedId == null) return null;
    for (final mark in widget.quickMarks) {
      if (mark.id == selectedId) return mark;
    }
    return null;
  }

  bool _isOnSelectedQuickMarkHandle(
    QuickMark mark,
    Rect rect,
    Offset position,
  ) {
    switch (mark.shape) {
      case QuickMarkShape.rectangle:
        return _isOnQuickMarkHandle(rect, position);
      case QuickMarkShape.arrow:
        return _isOnArrowEndpointHandle(rect, mark, position);
    }
  }

  Rect? _quickMarkLogicalRect(
    ViewportLayoutProjection projection,
    QuickMark mark,
    double devicePixelRatio,
  ) {
    final physicalRect = projection.viewportRectForSourceRect(
      mark.fileId,
      mark.sourceRect,
    );
    if (physicalRect == null || physicalRect.isEmpty) return null;
    return Rect.fromLTRB(
      physicalRect.left / devicePixelRatio,
      physicalRect.top / devicePixelRatio,
      physicalRect.right / devicePixelRatio,
      physicalRect.bottom / devicePixelRatio,
    );
  }

  bool _isOnQuickMarkBorder(Rect rect, Offset position) {
    final expanded = rect.inflate(_quickMarkBorderHitWidth / 2);
    final inner = rect.deflate(_quickMarkBorderHitWidth / 2);
    return expanded.contains(position) && !inner.contains(position);
  }

  bool _isOnQuickMarkInteractiveArea(
    ViewportLayoutProjection projection,
    QuickMark mark,
    Offset position,
    double devicePixelRatio,
  ) {
    final projected = projection.viewportProjectionForSourceRect(
      mark.fileId,
      mark.sourceRect,
    );
    if (projected == null || projected.clipRect.isEmpty) return false;
    final rect = Rect.fromLTRB(
      projected.viewportRect.left / devicePixelRatio,
      projected.viewportRect.top / devicePixelRatio,
      projected.viewportRect.right / devicePixelRatio,
      projected.viewportRect.bottom / devicePixelRatio,
    );
    final clipRect = Rect.fromLTRB(
      projected.clipRect.left / devicePixelRatio,
      projected.clipRect.top / devicePixelRatio,
      projected.clipRect.right / devicePixelRatio,
      projected.clipRect.bottom / devicePixelRatio,
    );
    final textRect = quickMarkTextHitRect(mark, rect, clipRect);
    if (textRect != null && textRect.contains(position)) return true;
    if (_isOnQuickMarkProjectedShape(mark, rect, clipRect, position)) {
      return true;
    }
    if (!mark.syncAcrossTracks || widget.trackGeometry.length < 2) {
      return false;
    }
    for (final track in widget.trackGeometry) {
      if (track.fileId == mark.fileId) continue;
      final syncedProjected = projection.viewportProjectionForSourceRect(
        track.fileId,
        mark.sourceRect,
      );
      if (syncedProjected == null || syncedProjected.clipRect.isEmpty) {
        continue;
      }
      final syncedRect = Rect.fromLTRB(
        syncedProjected.viewportRect.left / devicePixelRatio,
        syncedProjected.viewportRect.top / devicePixelRatio,
        syncedProjected.viewportRect.right / devicePixelRatio,
        syncedProjected.viewportRect.bottom / devicePixelRatio,
      );
      final syncedClipRect = Rect.fromLTRB(
        syncedProjected.clipRect.left / devicePixelRatio,
        syncedProjected.clipRect.top / devicePixelRatio,
        syncedProjected.clipRect.right / devicePixelRatio,
        syncedProjected.clipRect.bottom / devicePixelRatio,
      );
      if (_isOnQuickMarkProjectedShape(
        mark,
        syncedRect,
        syncedClipRect,
        position,
      )) {
        return true;
      }
    }
    return false;
  }

  bool _isOnQuickMarkProjectedShape(
    QuickMark mark,
    Rect rect,
    Rect clipRect,
    Offset position,
  ) {
    switch (mark.shape) {
      case QuickMarkShape.rectangle:
        return _isOnQuickMarkBorder(rect.intersect(clipRect), position);
      case QuickMarkShape.arrow:
        return _isOnQuickMarkArrow(rect, mark, position);
    }
  }

  bool _isOnQuickMarkArrow(Rect rect, QuickMark mark, Offset position) {
    final start = _sourcePointToQuickMarkRect(
      rect,
      mark.sourceRect,
      mark.effectiveSourceStart,
    );
    final end = _sourcePointToQuickMarkRect(
      rect,
      mark.sourceRect,
      mark.effectiveSourceEnd,
    );
    final hitWidth = math.max(_quickMarkBorderHitWidth, mark.strokeWidth + 10);
    return _arrowSegments(start, end, mark.strokeWidth).any(
      (segment) =>
          _distanceToSegment(position, segment.start, segment.end) <=
          hitWidth / 2,
    );
  }

  List<({Offset start, Offset end})> _arrowSegments(
    Offset start,
    Offset end,
    double baseStrokeWidth,
  ) {
    final delta = end - start;
    if (delta.distance <= 0.1) {
      return [(start: start, end: end)];
    }
    final angle = math.atan2(delta.dy, delta.dx);
    final headLength = math.max(12.0, baseStrokeWidth * 4.0);
    const headAngle = math.pi / 7.0;
    final left = Offset(
      end.dx - math.cos(angle - headAngle) * headLength,
      end.dy - math.sin(angle - headAngle) * headLength,
    );
    final right = Offset(
      end.dx - math.cos(angle + headAngle) * headLength,
      end.dy - math.sin(angle + headAngle) * headLength,
    );
    return [
      (start: start, end: end),
      (start: end, end: left),
      (start: end, end: right),
    ];
  }

  double _distanceToSegment(Offset point, Offset start, Offset end) {
    final segment = end - start;
    final lengthSquared = segment.distanceSquared;
    if (lengthSquared <= 1e-6) return (point - start).distance;
    final pointDelta = point - start;
    final t =
        ((pointDelta.dx * segment.dx + pointDelta.dy * segment.dy) /
                lengthSquared)
            .clamp(0.0, 1.0)
            .toDouble();
    final nearest = Offset(
      start.dx + segment.dx * t,
      start.dy + segment.dy * t,
    );
    return (point - nearest).distance;
  }

  Offset _sourcePointToQuickMarkRect(
    Rect viewportRect,
    Rect sourceRect,
    Offset sourcePoint,
  ) {
    final tx = sourceRect.width.abs() <= 1e-6
        ? 0.5
        : (sourcePoint.dx - sourceRect.left) / sourceRect.width;
    final ty = sourceRect.height.abs() <= 1e-6
        ? 0.5
        : (sourcePoint.dy - sourceRect.top) / sourceRect.height;
    return Offset(
      viewportRect.left + viewportRect.width * tx,
      viewportRect.top + viewportRect.height * ty,
    );
  }

  bool _isOnQuickMarkHandle(Rect rect, Offset position) {
    final centers = [
      rect.topLeft,
      Offset(rect.center.dx, rect.top),
      rect.topRight,
      Offset(rect.right, rect.center.dy),
      rect.bottomRight,
      Offset(rect.center.dx, rect.bottom),
      rect.bottomLeft,
      Offset(rect.left, rect.center.dy),
    ];
    final half = _quickMarkHandleHitSize / 2;
    return centers.any(
      (center) => Rect.fromCenter(
        center: center,
        width: _quickMarkHandleHitSize,
        height: _quickMarkHandleHitSize,
      ).inflate(half).contains(position),
    );
  }

  bool _isOnArrowEndpointHandle(Rect rect, QuickMark mark, Offset position) {
    final centers = [
      _sourcePointToQuickMarkRect(
        rect,
        mark.sourceRect,
        mark.effectiveSourceStart,
      ),
      _sourcePointToQuickMarkRect(
        rect,
        mark.sourceRect,
        mark.effectiveSourceEnd,
      ),
    ];
    return centers.any(
      (center) => Rect.fromCenter(
        center: center,
        width: _quickMarkHandleHitSize,
        height: _quickMarkHandleHitSize,
      ).inflate(_quickMarkHandleHitSize / 2).contains(position),
    );
  }

  Rect _quickMarkPanelRect(Rect markRect, Size viewportSize) {
    final panelLeft = (markRect.center.dx - _quickMarkPanelWidth / 2)
        .clamp(
          8.0,
          math.max(8.0, viewportSize.width - _quickMarkPanelWidth - 8.0),
        )
        .toDouble();
    final belowTop = markRect.bottom + _quickMarkPanelGap;
    final aboveTop = markRect.top - _quickMarkPanelGap - _quickMarkPanelHeight;
    final panelTop = belowTop + _quickMarkPanelHeight <= viewportSize.height - 8
        ? belowTop
        : math.max(8.0, aboveTop);
    return Rect.fromLTWH(
      panelLeft,
      panelTop,
      _quickMarkPanelWidth,
      _quickMarkPanelHeight,
    );
  }

  void _startSplitHandleDrag(BuildContext context, double viewportLocalX) {
    _splitHandleDragging = true;
    _splitting = true;
    _panning = false;
    _lastMouseLocalPos = Offset(viewportLocalX, 0);
    widget.onPointerButton(false, true);
    _updateSplitFromLocalX(context, viewportLocalX);
  }

  void _updateSplitHandleDrag(BuildContext context, double viewportLocalX) {
    if (!_splitHandleDragging) return;
    _lastMouseLocalPos = Offset(viewportLocalX, 0);
    _updateSplitFromLocalX(context, viewportLocalX);
  }

  void _endSplitHandleDrag() {
    if (!_splitHandleDragging) return;
    _splitHandleDragging = false;
    _splitting = false;
    widget.onPointerButton(false, false);
  }

  bool _startQuickMarkDrag(Offset localPosition, double devicePixelRatio) {
    final start = widget.onQuickMarkStart;
    if (start == null || _splitHandleDragging) return false;
    _quickMarkDragging = true;
    _panning = false;
    _splitting = false;
    _lastMouseLocalPos = localPosition;
    widget.onPointerButton(false, false);
    start(localPosition * devicePixelRatio);
    return true;
  }

  void _updateQuickMarkDrag(Offset localPosition, double devicePixelRatio) {
    if (!_quickMarkDragging) return;
    _lastMouseLocalPos = localPosition;
    widget.onQuickMarkUpdate?.call(localPosition * devicePixelRatio);
  }

  void _endQuickMarkDrag() {
    if (!_quickMarkDragging) return;
    _quickMarkDragging = false;
    widget.onQuickMarkEnd?.call();
  }

  void _cancelQuickMarkDrag() {
    if (!_quickMarkDragging) return;
    _quickMarkDragging = false;
    widget.onQuickMarkCancel?.call();
  }

  void _cancelPointerDragState() {
    if (!_panning &&
        !_splitting &&
        !_splitHandleDragging &&
        !_quickMarkDragging) {
      return;
    }
    final quickMarkDragging = _quickMarkDragging;
    _panning = false;
    _splitting = false;
    _splitHandleDragging = false;
    if (quickMarkDragging) {
      _cancelQuickMarkDrag();
    }
    widget.onPointerButton(false, false);
  }

  void _resetPanZoom() {
    _lastPanZoomScale = 1.0;
    _panZoomScaling = false;
  }

  void _zoomByFactor(double factor, Offset physicalLocalPosition) {
    if (factor <= 0 || !factor.isFinite || factor == 1.0) return;
    widget.onZoom(factor, physicalLocalPosition);
  }

  void _zoomByWheelDelta(double scrollDelta, Offset physicalLocalPosition) {
    if (scrollDelta == 0.0 || !scrollDelta.isFinite) return;
    final factor = math
        .pow(_wheelZoomFactorPerStep, -scrollDelta / _wheelScrollDeltaPerStep)
        .toDouble();
    _zoomByFactor(factor, physicalLocalPosition);
  }

  void _clampSplitOnExit(BuildContext context, Offset localPosition) {
    if (!_splitting || widget.layout.mode != LayoutMode.splitScreen) return;
    final box = context.findRenderObject() as RenderBox;
    final width = box.size.width;
    if (width <= 0) return;

    if (localPosition.dx <= 0.0) {
      widget.onSplit(0.0);
    } else if (localPosition.dx >= width) {
      widget.onSplit(1.0);
    }
  }

  void _maybeReportResize(
    BuildContext context, {
    required double logicalWidth,
    required double logicalHeight,
  }) {
    final logicalSize = Size(logicalWidth, logicalHeight);
    final devicePixelRatio = View.of(context).devicePixelRatio;
    if ((logicalSize != _lastReportedLogicalSize ||
            devicePixelRatio != _lastReportedDevicePixelRatio) &&
        logicalWidth > 0 &&
        logicalHeight > 0) {
      _cancelPointerDragState();
      _lastReportedLogicalSize = logicalSize;
      _lastReportedDevicePixelRatio = devicePixelRatio;
      final physicalWidth = (logicalWidth * devicePixelRatio).round();
      final physicalHeight = (logicalHeight * devicePixelRatio).round();
      _debugResizeReportCount++;
      final now = DateTime.now();
      final lastResizeLog = _lastResizePacingLogAt;
      final shouldLogResize =
          _debugResizeReportCount <= 8 ||
          lastResizeLog == null ||
          now.difference(lastResizeLog) >= _resizePacingLogInterval;
      if (shouldLogResize) {
        _lastResizePacingLogAt = now;
        log.info(
          '[WindowsResizePacing] flutter viewportReport '
          'count=$_debugResizeReportCount '
          'logical=${logicalWidth.toStringAsFixed(1)}x'
          '${logicalHeight.toStringAsFixed(1)} '
          'physical=${physicalWidth}x$physicalHeight '
          'dpr=${devicePixelRatio.toStringAsFixed(3)} '
          'nativeHole=${widget.nativeCompositorHole} '
          'texture=${widget.textureId}',
        );
      }
      log.fine(
        '[WindowsCompositorDebug] viewport resize report '
        'logical=${logicalWidth.toStringAsFixed(1)}x'
        '${logicalHeight.toStringAsFixed(1)} '
        'physical=${physicalWidth}x$physicalHeight '
        'dpr=${devicePixelRatio.toStringAsFixed(3)} '
        'nativeHole=${widget.nativeCompositorHole} '
        'texture=${widget.textureId}',
      );
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted) return;
        widget.onResize?.call(physicalWidth, physicalHeight, devicePixelRatio);
      });
    }
    if (widget.nativeCompositorHole) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted) return;
        _maybeReportNativeCompositorViewportRect(context);
      });
    }
  }

  void _maybeReportNativeCompositorViewportRect(BuildContext context) {
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) return;
    final view = View.of(context);
    final devicePixelRatio = view.devicePixelRatio;
    if (devicePixelRatio <= 0) return;
    final globalOffset = renderObject.localToGlobal(Offset.zero);
    final logicalSize = renderObject.size;
    final surfaceSize = view.physicalSize;
    if (logicalSize.width <= 0 ||
        logicalSize.height <= 0 ||
        surfaceSize.width <= 0 ||
        surfaceSize.height <= 0) {
      return;
    }
    if (globalOffset == _lastReportedGlobalOffset &&
        logicalSize == _lastReportedCompositorLogicalSize &&
        surfaceSize == _lastReportedSurfaceSize &&
        devicePixelRatio == _lastReportedDevicePixelRatio) {
      return;
    }
    _lastReportedGlobalOffset = globalOffset;
    _lastReportedCompositorLogicalSize = logicalSize;
    _lastReportedSurfaceSize = surfaceSize;
    final left = (globalOffset.dx * devicePixelRatio).round();
    final top = (globalOffset.dy * devicePixelRatio).round();
    final width = (logicalSize.width * devicePixelRatio).round();
    final height = (logicalSize.height * devicePixelRatio).round();
    final surfaceWidth = surfaceSize.width.round();
    final surfaceHeight = surfaceSize.height.round();
    _debugViewportRectReportCount++;
    final now = DateTime.now();
    final lastRectLog = _lastViewportRectPacingLogAt;
    final shouldLogRect =
        _debugViewportRectReportCount <= 8 ||
        lastRectLog == null ||
        now.difference(lastRectLog) >= _resizePacingLogInterval;
    if (shouldLogRect) {
      _lastViewportRectPacingLogAt = now;
      log.info(
        '[WindowsResizePacing] flutter viewportRect '
        'count=$_debugViewportRectReportCount '
        'physical=($left,$top ${width}x$height) '
        'surface=${surfaceWidth}x$surfaceHeight '
        'logicalOffset=(${globalOffset.dx.toStringAsFixed(1)},'
        '${globalOffset.dy.toStringAsFixed(1)}) '
        'logicalSize=${logicalSize.width.toStringAsFixed(1)}x'
        '${logicalSize.height.toStringAsFixed(1)} '
        'dpr=${devicePixelRatio.toStringAsFixed(3)}',
      );
    }
    log.fine(
      '[WindowsCompositorDebug] native compositor viewport rect '
      'physical=($left,$top ${width}x$height) '
      'surface=${surfaceWidth}x$surfaceHeight '
      'logicalOffset=(${globalOffset.dx.toStringAsFixed(1)},'
      '${globalOffset.dy.toStringAsFixed(1)}) '
      'logicalSize=${logicalSize.width.toStringAsFixed(1)}x'
      '${logicalSize.height.toStringAsFixed(1)} '
      'dpr=${devicePixelRatio.toStringAsFixed(3)}',
    );
    widget.onNativeCompositorViewportRect?.call(
      left,
      top,
      width,
      height,
      surfaceWidth,
      surfaceHeight,
    );
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        _maybeReportResize(
          context,
          logicalWidth: constraints.maxWidth,
          logicalHeight: constraints.maxHeight,
        );
        return IndexedStack(
          index: widget.viewportState.stackIndex,
          sizing: StackFit.expand,
          children: [
            // State 0: Loading
            Center(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  SizedBox(
                    width: 48,
                    height: 48,
                    child: CircularProgressIndicator(
                      strokeWidth: 3,
                      color: Theme.of(context).colorScheme.primary,
                    ),
                  ),
                  const SizedBox(height: 8),
                  Text(
                    AppLocalizations.of(context)!.initializing,
                    style: TextStyle(
                      color: Theme.of(context).colorScheme.onSurfaceVariant,
                    ),
                  ),
                ],
              ),
            ),
            // State 1: Empty
            Center(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(
                    Icons.videocam_outlined,
                    size: 64,
                    color: Theme.of(context).colorScheme.onSurfaceVariant,
                  ),
                  const SizedBox(height: 8),
                  Text(
                    widget.nativePlaybackAvailable
                        ? AppLocalizations.of(context)!.emptyHint
                        : AppLocalizations.of(
                            context,
                          )!.platformPlaybackUnavailable,
                    style: TextStyle(
                      color: Theme.of(context).colorScheme.onSurfaceVariant,
                    ),
                  ),
                ],
              ),
            ),
            // State 2: Active (Texture + mouse listener)
            _buildActiveViewport(context),
            // State 3: Error
            Center(
              child: Padding(
                padding: const EdgeInsets.all(24),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Icon(
                      Icons.error_outline,
                      size: 56,
                      color: Theme.of(context).colorScheme.error,
                    ),
                    const SizedBox(height: 10),
                    Text(
                      widget.errorText ?? 'Failed to load media',
                      textAlign: TextAlign.center,
                      style: TextStyle(
                        color: Theme.of(context).colorScheme.onSurfaceVariant,
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ],
        );
      },
    );
  }

  Widget _buildActiveViewport(BuildContext context) {
    if (widget.textureId == null) {
      return const SizedBox.shrink();
    }
    final devicePixelRatio = View.of(context).devicePixelRatio;
    return AxTreeRegion(
      label: 'Video viewport',
      value: widget.layout.mode == LayoutMode.splitScreen
          ? 'Split ${axPercent(widget.layout.splitPos)}'
          : null,
      image: true,
      child: MouseRegion(
        onEnter: (e) {
          _lastMouseLocalPos = e.localPosition;
          _syncDragButtons(
            e.buttons,
            e.localPosition,
            allowWin32Recovery: true,
          );
        },
        onExit: (e) => _clampSplitOnExit(context, e.localPosition),
        onHover: (e) {
          _debugPointerHoverCount++;
          if (!_panning && !_splitting) {
            _lastMouseLocalPos = e.localPosition;
          }
          _syncDragButtons(e.buttons, e.localPosition);
          _updateSplitFromLocalX(context, e.localPosition.dx);
          _logDebugInteractionSample('hover', e);
        },
        child: Listener(
          behavior: HitTestBehavior.opaque,
          onPointerDown: (e) {
            log.fine(
              '[WindowsCompositorDebug] viewport pointerDown '
              'kind=${e.kind.name} buttons=${e.buttons} '
              'local=(${e.localPosition.dx.toStringAsFixed(1)},'
              '${e.localPosition.dy.toStringAsFixed(1)}) '
              'nativeHole=${widget.nativeCompositorHole} '
              'state=${widget.viewportState.status.name} '
              'texture=${widget.textureId}',
            );
            if ((e.buttons & kPrimaryButton) != 0) {
              if (_isOnSplitHandle(context, e.localPosition)) {
                _startSplitHandleDrag(context, e.localPosition.dx);
                return;
              }
              if (_isOnQuickMarkEditor(
                context,
                e.localPosition,
                devicePixelRatio,
              )) {
                return;
              }
              _panning = false;
              _splitting = false;
              _lastMouseLocalPos = e.localPosition;
              if (_startQuickMarkDrag(e.localPosition, devicePixelRatio)) {
                return;
              }
            }
            _syncDragButtons(e.buttons, e.localPosition);
            _updateSplitFromLocalX(context, e.localPosition.dx);
          },
          onPointerUp: (e) {
            log.fine(
              '[WindowsCompositorDebug] viewport pointerUp '
              'kind=${e.kind.name} buttons=${e.buttons} '
              'local=(${e.localPosition.dx.toStringAsFixed(1)},'
              '${e.localPosition.dy.toStringAsFixed(1)}) '
              'panning=$_panning splitting=$_splitting '
              'quickMark=$_quickMarkDragging splitHandle=$_splitHandleDragging',
            );
            if (_quickMarkDragging &&
                !widget.interactionPolicy.isPrimaryButtonDown(e.buttons)) {
              _endQuickMarkDrag();
              return;
            }
            if (_splitHandleDragging) {
              _endSplitHandleDrag();
              return;
            }
            _syncDragButtons(
              e.buttons,
              e.localPosition,
              allowWin32Recovery: true,
            );
          },
          onPointerCancel: (_) {
            log.fine(
              '[WindowsCompositorDebug] viewport pointerCancel '
              'panning=$_panning splitting=$_splitting '
              'quickMark=$_quickMarkDragging splitHandle=$_splitHandleDragging',
            );
            if (_quickMarkDragging) {
              _cancelQuickMarkDrag();
              return;
            }
            if (_splitHandleDragging) {
              _endSplitHandleDrag();
              return;
            }
            _syncDragButtons(0, _lastMouseLocalPos, allowWin32Recovery: true);
          },
          onPointerMove: (e) {
            _debugPointerMoveCount++;
            if (_quickMarkDragging) {
              if (!widget.interactionPolicy.isPrimaryButtonDown(e.buttons)) {
                _endQuickMarkDrag();
              } else {
                _updateQuickMarkDrag(e.localPosition, devicePixelRatio);
                _logDebugInteractionSample('quick-mark-drag', e);
              }
              return;
            }
            if (_splitHandleDragging) {
              if ((e.buttons & kPrimaryButton) == 0) {
                _endSplitHandleDrag();
              } else {
                _updateSplitHandleDrag(context, e.localPosition.dx);
                _logDebugInteractionSample('split-handle-drag', e);
              }
              return;
            }
            _syncDragButtons(
              e.buttons,
              e.localPosition,
              allowWin32Recovery: _panning || _splitting,
            );
            if (!_panning && !_splitting) {
              _logDebugInteractionSample('move-ignored', e);
              return;
            }
            final logicalDelta = e.localPosition - _lastMouseLocalPos;
            final physicalDelta = logicalDelta * devicePixelRatio;
            _lastMouseLocalPos = e.localPosition;

            if (_panning) {
              ViewportProjectionDiagnostics.instance.record(
                'pointerMovePanDispatch',
              );
              widget.onPan(physicalDelta);
            }

            _updateSplitFromLocalX(context, e.localPosition.dx);
            _logDebugInteractionSample(
              _panning ? 'pan' : 'split',
              e,
              logicalDelta: logicalDelta,
              physicalDelta: physicalDelta,
            );
          },
          onPointerSignal: (e) {
            if (e is PointerScrollEvent) {
              _zoomByWheelDelta(
                e.scrollDelta.dy,
                e.localPosition * devicePixelRatio,
              );
            }
          },
          onPointerPanZoomStart: (_) => _resetPanZoom(),
          onPointerPanZoomUpdate: (e) {
            ViewportProjectionDiagnostics.instance.record(
              'pointerPanZoomUpdate',
            );
            if (e.scale > 0 && e.scale.isFinite && _lastPanZoomScale > 0) {
              final previousScale = _lastPanZoomScale;
              final scaleDelta = e.scale / _lastPanZoomScale;
              _lastPanZoomScale = e.scale;
              final scaleIntent =
                  _panZoomScaling ||
                  isPanZoomScaleIntent(
                    scale: e.scale,
                    lastScale: previousScale,
                  );
              if (scaleIntent && scaleDelta != 1.0) {
                _panZoomScaling = true;
                ViewportProjectionDiagnostics.instance.record(
                  'pointerPanZoomScaleDispatch',
                );
                _zoomByFactor(scaleDelta, e.localPosition * devicePixelRatio);
                _logDebugInteractionSample(
                  'pan-zoom-scale',
                  e,
                  scaleDelta: scaleDelta,
                );
                return;
              }
            }

            if (_panZoomScaling) return;
            final physicalPanDelta = e.panDelta * devicePixelRatio;
            if (physicalPanDelta != Offset.zero) {
              ViewportProjectionDiagnostics.instance.record(
                'pointerPanZoomPanDispatch',
              );
              widget.onPan(physicalPanDelta);
              _logDebugInteractionSample(
                'pan-zoom-pan',
                e,
                physicalDelta: physicalPanDelta,
              );
            }
          },
          onPointerPanZoomEnd: (_) => _resetPanZoom(),
          child: Stack(
            fit: StackFit.expand,
            children: [
              if (!widget.nativeCompositorHole)
                ExcludeSemantics(child: Texture(textureId: widget.textureId!)),
              QuickMarkOverlay(
                layout: widget.layout,
                tracks: widget.trackGeometry,
                marks: widget.quickMarks,
                draft: widget.quickMarkDraft,
                selectedMarkId: widget.selectedQuickMarkId,
                devicePixelRatio: devicePixelRatio,
                onSelectedMarkChanged: widget.onQuickMarkSelect,
                onMarkChanged: widget.onQuickMarkChanged,
                onInteraction: widget.onQuickMarkInteraction,
                onMarkDeleted: widget.onQuickMarkDeleted,
                onMarkFocus: widget.onQuickMarkFocus,
              ),
              if (widget.layout.mode == LayoutMode.splitScreen)
                _buildSplitHandleSemantics(context, devicePixelRatio),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildSplitHandleSemantics(
    BuildContext context,
    double devicePixelRatio,
  ) {
    final splitPos = widget.layout.splitPos;
    return Positioned.fill(
      child: Semantics(
        container: true,
        slider: true,
        label: 'Viewport split handle',
        value: axPercent(splitPos),
        increasedValue: axPercent((splitPos + 0.05).clamp(0.0, 1.0)),
        decreasedValue: axPercent((splitPos - 0.05).clamp(0.0, 1.0)),
        onIncrease: () => widget.onSplit((splitPos + 0.05).clamp(0.0, 1.0)),
        onDecrease: () => widget.onSplit((splitPos - 0.05).clamp(0.0, 1.0)),
        child: ExcludeSemantics(
          child: _buildSplitHandle(context, devicePixelRatio),
        ),
      ),
    );
  }

  Widget _buildSplitHandle(BuildContext context, double devicePixelRatio) {
    final viewportContext = context;
    final logicalWidth = (_splitHandlePhysicalWidth / devicePixelRatio).clamp(
      2.0,
      4.0,
    );
    return LayoutBuilder(
      builder: (context, constraints) {
        final left =
            constraints.maxWidth * widget.layout.splitPos - logicalWidth / 2;
        final touchLeft =
            constraints.maxWidth * widget.layout.splitPos -
            _splitHandleTouchWidth / 2;
        return Stack(
          children: [
            Positioned(
              left: left,
              top: 0,
              bottom: 0,
              width: logicalWidth,
              child: IgnorePointer(
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: Theme.of(
                      context,
                    ).colorScheme.outline.withValues(alpha: 0.55),
                  ),
                ),
              ),
            ),
            Positioned(
              left: touchLeft,
              top: 0,
              bottom: 0,
              width: _splitHandleTouchWidth,
              child: MouseRegion(
                cursor: SystemMouseCursors.resizeColumn,
                child: Listener(
                  behavior: HitTestBehavior.translucent,
                  onPointerDown: (event) {
                    if ((event.buttons & kPrimaryButton) == 0) return;
                    final localX = _viewportLocalXFromGlobal(
                      viewportContext,
                      event.position,
                    );
                    if (localX == null) return;
                    _startSplitHandleDrag(viewportContext, localX);
                  },
                  onPointerMove: (event) {
                    if ((event.buttons & kPrimaryButton) == 0) {
                      _endSplitHandleDrag();
                    }
                  },
                  child: Center(child: _SplitHandleGrip()),
                ),
              ),
            ),
          ],
        );
      },
    );
  }
}

class _SplitHandleGrip extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Container(
      width: _ViewportPanelState._splitHandleVisualWidth,
      height: _ViewportPanelState._splitHandleVisualHeight,
      decoration: BoxDecoration(
        color: colorScheme.surfaceContainerHighest.withValues(alpha: 0.82),
        borderRadius: BorderRadius.circular(6),
        border: Border.all(
          color: colorScheme.outlineVariant.withValues(alpha: 0.7),
        ),
        boxShadow: [
          BoxShadow(
            color: Colors.black.withValues(alpha: 0.18),
            blurRadius: 8,
            offset: const Offset(0, 2),
          ),
        ],
      ),
      child: RotatedBox(
        quarterTurns: 1,
        child: Icon(
          Icons.drag_handle,
          size: 18,
          color: colorScheme.onSurfaceVariant,
        ),
      ),
    );
  }
}
