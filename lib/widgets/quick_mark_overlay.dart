import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../l10n/app_localizations.dart';
import '../marks/quick_mark.dart';
import '../video_renderer_controller.dart';
import '../viewport/display_geometry.dart';
import 'app_menu_combo.dart';

enum _MarkHandle {
  topLeft,
  top,
  topRight,
  right,
  bottomRight,
  bottom,
  bottomLeft,
  left,
}

enum _ArrowEndpoint { start, end }

class _CancelTextEditIntent extends Intent {
  const _CancelTextEditIntent();
}

class _CommitTextEditIntent extends Intent {
  const _CommitTextEditIntent();
}

class _LogicalProjectedMark {
  final Rect viewportRect;
  final Rect clipRect;

  const _LogicalProjectedMark({
    required this.viewportRect,
    required this.clipRect,
  });

  bool get isVisible => !clipRect.isEmpty && viewportRect.overlaps(clipRect);
}

class _QuickMarkTextLayout {
  final Rect rect;
  final double textMaxWidth;
  final TextStyle style;

  const _QuickMarkTextLayout({
    required this.rect,
    required this.textMaxWidth,
    required this.style,
  });
}

TextStyle _quickMarkTextStyle(QuickMark mark) {
  return TextStyle(
    color: mark.color,
    fontSize: mark.textFontSize,
    height: 1.18,
    fontWeight: mark.textBold ? FontWeight.w700 : FontWeight.w400,
  );
}

_QuickMarkTextLayout? _quickMarkTextLayout(
  QuickMark mark,
  Rect markRect,
  Rect clipRect, {
  required String text,
}) {
  final expandedClip = markRect
      .inflate(_QuickMarkOverlayState._textMaxWidth)
      .intersect(clipRect);
  if (expandedClip.isEmpty) return null;

  final style = _quickMarkTextStyle(mark);
  final maxLayoutWidth = math.min(
    _QuickMarkOverlayState._textMaxWidth,
    mark.shape == QuickMarkShape.rectangle
        ? math.max(8.0, markRect.width)
        : _QuickMarkOverlayState._textMaxWidth,
  );
  final availableTextWidth = math.max(
    8.0,
    math.min(maxLayoutWidth, expandedClip.width),
  );
  final painter = TextPainter(
    text: TextSpan(text: text.isEmpty ? ' ' : text, style: style),
    maxLines: 5,
    ellipsis: '…',
    textDirection: TextDirection.ltr,
  )..layout(maxWidth: availableTextWidth);

  final contentWidth = math.min(
    expandedClip.width,
    math.max(1.0, painter.width),
  );
  final width = math.min(
    expandedClip.width,
    contentWidth + _QuickMarkOverlayState._textEditGutter,
  );
  final height = math.min(
    expandedClip.height,
    math.min(_QuickMarkOverlayState._textMaxHeight, painter.height),
  );
  if (width <= 0 || height <= 0) return null;

  final anchor = mark.shape == QuickMarkShape.arrow
      ? _quickMarkArrowTextAnchor(mark, markRect)
      : null;
  final rawLeft = anchor == null ? markRect.left : anchor.dx - contentWidth / 2;
  final rawTop = anchor == null
      ? markRect.top - _QuickMarkOverlayState._textGap - height
      : anchor.dy - height / 2;
  final left = rawLeft.clamp(
    expandedClip.left,
    math.max(expandedClip.left, expandedClip.right - width),
  );
  final top = rawTop.clamp(
    expandedClip.top,
    math.max(expandedClip.top, expandedClip.bottom - height),
  );
  return _QuickMarkTextLayout(
    rect: Rect.fromLTWH(left.toDouble(), top.toDouble(), width, height),
    textMaxWidth: math.max(8.0, contentWidth),
    style: style,
  );
}

Rect? quickMarkTextHitRect(
  QuickMark mark,
  Rect markRect,
  Rect clipRect, {
  String? text,
}) {
  if ((text ?? mark.text).trim().isEmpty) return null;
  return _quickMarkTextLayout(
    mark,
    markRect,
    clipRect,
    text: text ?? mark.text,
  )?.rect;
}

Offset _quickMarkArrowTextAnchor(QuickMark mark, Rect rect) {
  final start = _quickMarkSourcePointToViewportRect(
    rect,
    mark.sourceRect,
    mark.effectiveSourceStart,
  );
  final end = _quickMarkSourcePointToViewportRect(
    rect,
    mark.sourceRect,
    mark.effectiveSourceEnd,
  );
  return Offset.lerp(start, end, 0.5)!;
}

Offset _quickMarkSourcePointToViewportRect(
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

class QuickMarkOverlay extends StatefulWidget {
  final LayoutState layout;
  final List<DisplayTrackGeometry> tracks;
  final List<QuickMark> marks;
  final QuickMark? draft;
  final int? selectedMarkId;
  final double devicePixelRatio;
  final ValueChanged<int?>? onSelectedMarkChanged;
  final ValueChanged<QuickMark>? onMarkChanged;
  final VoidCallback? onInteraction;
  final ValueChanged<int>? onMarkDeleted;
  final ValueChanged<int>? onMarkFocus;

  const QuickMarkOverlay({
    super.key,
    required this.layout,
    required this.tracks,
    required this.marks,
    required this.draft,
    required this.selectedMarkId,
    required this.devicePixelRatio,
    this.onSelectedMarkChanged,
    this.onMarkChanged,
    this.onInteraction,
    this.onMarkDeleted,
    this.onMarkFocus,
  });

  @override
  State<QuickMarkOverlay> createState() => _QuickMarkOverlayState();
}

class _QuickMarkOverlayState extends State<QuickMarkOverlay> {
  static const double _borderHitWidth = 12.0;
  static const double _handleHitSize = 22.0;
  static const double _panelButtonSize = 28.0;
  static const double _panelHeight = 36.0;
  static const double _panelGap = 10.0;
  static const double _textMaxWidth = 220.0;
  static const double _textMaxHeight = 128.0;
  static const double _textGap = 4.0;
  static const double _textEditGutter = 10.0;
  static const List<double> _fontSizes = [
    10.0,
    12.0,
    14.0,
    16.0,
    18.0,
    20.0,
    24.0,
    28.0,
    32.0,
  ];
  static const List<Color> _colors = [
    Color(0xFFFF3B30),
    Color(0xFFFF9500),
    Color(0xFFFFCC00),
    Color(0xFF34C759),
    Color(0xFF00C7BE),
    Color(0xFF0A84FF),
    Color(0xFFBF5AF2),
    Color(0xFFFFFFFF),
    Color(0xFF000000),
  ];
  static const List<double> _strokeWidths = [1.0, 2.0, 3.0, 5.0, 8.0];

  QuickMark? _resizeStartMark;
  QuickMark? _moveStartMark;
  Offset? _moveStartSourceUv;
  _ArrowEndpoint? _activeArrowEndpoint;
  int? _editingTextMarkId;
  int? _lastPointerDownMarkId;
  Duration? _lastPointerDownTime;
  Offset? _lastPointerDownPosition;
  late final TextEditingController _textController;
  late final FocusNode _textFocusNode;

  @override
  void initState() {
    super.initState();
    _textController = TextEditingController();
    _textFocusNode = FocusNode();
    _textFocusNode.addListener(_handleTextFocusChanged);
  }

  @override
  void didUpdateWidget(covariant QuickMarkOverlay oldWidget) {
    super.didUpdateWidget(oldWidget);
    final editingId = _editingTextMarkId;
    if (editingId == null) return;
    final mark = _markById(editingId);
    if (mark == null || widget.selectedMarkId != editingId) {
      _stopEditingText(commit: false);
      return;
    }
    if (oldWidget.marks != widget.marks &&
        _textController.text != mark.text &&
        !_textFocusNode.hasFocus) {
      _textController.text = mark.text;
    }
  }

  @override
  void dispose() {
    _textFocusNode.removeListener(_handleTextFocusChanged);
    _textFocusNode.dispose();
    _textController.dispose();
    super.dispose();
  }

  void _handleTextFocusChanged() {
    if (!_textFocusNode.hasFocus) {
      _stopEditingText(commit: true);
    }
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final projection = computeViewportLayoutProjection(
          viewportWidth: (constraints.maxWidth * widget.devicePixelRatio)
              .round(),
          viewportHeight: (constraints.maxHeight * widget.devicePixelRatio)
              .round(),
          layout: widget.layout,
          tracks: widget.tracks,
        );
        final selected = _selectedVisibleMark;
        final selectedProjection = selected == null
            ? null
            : _logicalViewportProjection(projection, selected);
        return Stack(
          fit: StackFit.expand,
          clipBehavior: Clip.none,
          children: [
            IgnorePointer(
              child: CustomPaint(
                painter: _QuickMarkPainter(
                  projection: projection,
                  tracks: widget.tracks,
                  marks: widget.marks,
                  draft: widget.draft,
                  selectedMarkId: widget.selectedMarkId,
                  editingTextMarkId: _editingTextMarkId,
                  devicePixelRatio: widget.devicePixelRatio,
                ),
              ),
            ),
            for (final mark in widget.marks)
              ..._buildSyncedMarkHitTargets(context, projection, mark),
            for (final mark in widget.marks)
              ..._buildMarkHitTargets(context, projection, mark),
            for (final mark in widget.marks)
              ..._buildMarkTextHitTarget(context, projection, mark),
            if (selected != null && selectedProjection != null)
              ..._buildSelectedMarkControls(
                context,
                projection,
                selected,
                selectedProjection.viewportRect,
                selectedProjection.clipRect,
                constraints.biggest,
              ),
            if (selected != null &&
                selectedProjection != null &&
                _editingTextMarkId == selected.id)
              _buildTextEditor(
                context,
                selected,
                selectedProjection.viewportRect,
                selectedProjection.clipRect,
              ),
          ],
        );
      },
    );
  }

  QuickMark? get _selectedVisibleMark {
    final selectedId = widget.selectedMarkId;
    if (selectedId == null) return null;
    return _markById(selectedId);
  }

  void _notifyInteraction() {
    widget.onInteraction?.call();
  }

  QuickMark? _markById(int id) {
    for (final mark in widget.marks) {
      if (mark.id == id) return mark;
    }
    return null;
  }

  void _startEditingText(QuickMark mark) {
    _notifyInteraction();
    widget.onSelectedMarkChanged?.call(mark.id);
    if (_editingTextMarkId != mark.id) {
      _textController.text = mark.text;
      _textController.selection = TextSelection.collapsed(
        offset: _textController.text.length,
      );
    }
    setState(() => _editingTextMarkId = mark.id);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || _editingTextMarkId != mark.id) return;
      _textFocusNode.requestFocus();
    });
  }

  void _stopEditingText({required bool commit}) {
    final editingId = _editingTextMarkId;
    if (editingId == null) return;
    if (commit) {
      final mark = _markById(editingId);
      if (mark != null && mark.text != _textController.text) {
        _notifyInteraction();
        widget.onMarkChanged?.call(mark.copyWith(text: _textController.text));
      }
    }
    if (mounted) {
      setState(() => _editingTextMarkId = null);
    } else {
      _editingTextMarkId = null;
    }
  }

  List<Widget> _buildMarkHitTargets(
    BuildContext context,
    ViewportLayoutProjection projection,
    QuickMark mark,
  ) {
    final projected = _logicalViewportProjection(projection, mark);
    if (projected == null || !projected.isVisible) return const [];
    if (mark.shape == QuickMarkShape.arrow) {
      return _buildArrowHitTargets(
        context,
        projection,
        mark,
        projected.viewportRect,
        projected.clipRect,
        mark.id == widget.selectedMarkId
            ? _arrowEndpointHitRects(mark, projected.viewportRect)
            : const [],
      );
    }
    final rect = projected.viewportRect.intersect(projected.clipRect);
    if (rect.isEmpty) return const [];
    final halfHit = _borderHitWidth / 2;
    return [
      _buildBorderHitTarget(
        context,
        projection,
        mark,
        Rect.fromLTWH(
          rect.left,
          rect.top - halfHit,
          rect.width,
          _borderHitWidth,
        ),
      ),
      _buildBorderHitTarget(
        context,
        projection,
        mark,
        Rect.fromLTWH(
          rect.left,
          rect.bottom - halfHit,
          rect.width,
          _borderHitWidth,
        ),
      ),
      _buildBorderHitTarget(
        context,
        projection,
        mark,
        Rect.fromLTWH(
          rect.left - halfHit,
          rect.top,
          _borderHitWidth,
          rect.height,
        ),
      ),
      _buildBorderHitTarget(
        context,
        projection,
        mark,
        Rect.fromLTWH(
          rect.right - halfHit,
          rect.top,
          _borderHitWidth,
          rect.height,
        ),
      ),
    ];
  }

  List<Widget> _buildSyncedMarkHitTargets(
    BuildContext context,
    ViewportLayoutProjection projection,
    QuickMark mark,
  ) {
    if (!mark.syncAcrossTracks || widget.tracks.length < 2) return const [];
    final targets = <Widget>[];
    for (final track in widget.tracks) {
      if (track.fileId == mark.fileId) continue;
      final projected = _logicalViewportProjectionForFile(
        projection,
        mark,
        track.fileId,
      );
      if (projected == null || !projected.isVisible) continue;
      switch (mark.shape) {
        case QuickMarkShape.rectangle:
          targets.addAll(
            _buildSyncedRectangleHitTargets(
              mark,
              projected.viewportRect.intersect(projected.clipRect),
            ),
          );
        case QuickMarkShape.arrow:
          targets.addAll(
            _buildSyncedArrowHitTargets(
              mark,
              projected.viewportRect,
              projected.clipRect,
            ),
          );
      }
    }
    return targets;
  }

  List<Widget> _buildSyncedRectangleHitTargets(QuickMark mark, Rect rect) {
    if (rect.isEmpty) return const [];
    final halfHit = _borderHitWidth / 2;
    return [
      _buildSyncedHitTarget(
        mark,
        Rect.fromLTWH(
          rect.left,
          rect.top - halfHit,
          rect.width,
          _borderHitWidth,
        ),
      ),
      _buildSyncedHitTarget(
        mark,
        Rect.fromLTWH(
          rect.left,
          rect.bottom - halfHit,
          rect.width,
          _borderHitWidth,
        ),
      ),
      _buildSyncedHitTarget(
        mark,
        Rect.fromLTWH(
          rect.left - halfHit,
          rect.top,
          _borderHitWidth,
          rect.height,
        ),
      ),
      _buildSyncedHitTarget(
        mark,
        Rect.fromLTWH(
          rect.right - halfHit,
          rect.top,
          _borderHitWidth,
          rect.height,
        ),
      ),
    ];
  }

  List<Widget> _buildSyncedArrowHitTargets(
    QuickMark mark,
    Rect viewportRect,
    Rect clipRect,
  ) {
    final start = _sourcePointToViewportRect(
      viewportRect,
      mark.sourceRect,
      mark.effectiveSourceStart,
    );
    final end = _sourcePointToViewportRect(
      viewportRect,
      mark.sourceRect,
      mark.effectiveSourceEnd,
    );
    final segments = _arrowSegments(start, end, mark.strokeWidth);
    if (segments.isEmpty) return const [];

    final hitSize = math.max(_borderHitWidth, mark.strokeWidth + 10.0);
    final hitClip = clipRect.inflate(hitSize / 2);
    final targets = <Widget>[];
    for (final segment in segments) {
      final delta = segment.end - segment.start;
      final length = delta.distance;
      if (length <= 0.1) continue;
      final steps = math.max(1, (length / (hitSize * 0.55)).ceil());
      for (var i = 0; i <= steps; i += 1) {
        final center = Offset.lerp(segment.start, segment.end, i / steps)!;
        if (!hitClip.contains(center)) continue;
        targets.add(
          _buildSyncedHitTarget(
            mark,
            Rect.fromCenter(center: center, width: hitSize, height: hitSize),
          ),
        );
      }
    }
    return targets;
  }

  Widget _buildSyncedHitTarget(QuickMark mark, Rect rect) {
    return Positioned.fromRect(
      rect: rect,
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        child: GestureDetector(
          behavior: HitTestBehavior.translucent,
          onTap: () {
            _notifyInteraction();
            widget.onSelectedMarkChanged?.call(mark.id);
          },
          child: const SizedBox.expand(),
        ),
      ),
    );
  }

  List<Widget> _buildMarkTextHitTarget(
    BuildContext context,
    ViewportLayoutProjection projection,
    QuickMark mark,
  ) {
    if (mark.text.trim().isEmpty || _editingTextMarkId == mark.id) {
      return const [];
    }
    final projected = _logicalViewportProjection(projection, mark);
    if (projected == null || !projected.isVisible) return const [];
    final layout = _textLayoutForMark(
      mark,
      projected.viewportRect,
      projected.clipRect,
      text: mark.text,
    );
    if (layout == null || layout.rect.isEmpty) return const [];
    return [
      _buildBorderHitTarget(
        context,
        projection,
        mark,
        layout.rect,
        key: ValueKey('quick-mark-text-hit-${mark.id}'),
      ),
    ];
  }

  Widget _buildBorderHitTarget(
    BuildContext context,
    ViewportLayoutProjection projection,
    QuickMark mark,
    Rect rect, {
    Key? key,
  }) {
    return Positioned.fromRect(
      key: key,
      rect: rect,
      child: MouseRegion(
        cursor: SystemMouseCursors.grab,
        child: Listener(
          behavior: HitTestBehavior.translucent,
          onPointerDown: (event) {
            if (_consumeDoubleClick(mark, event.timeStamp, event.position)) {
              _startEditingText(mark);
              return;
            }
            _startMoveMark(context, projection, mark, event.position);
          },
          onPointerUp: (_) => _endMoveMark(),
          onPointerCancel: (_) => _endMoveMark(),
          child: GestureDetector(
            behavior: HitTestBehavior.translucent,
            onPanUpdate: (details) => _moveMarkFromGlobalPosition(
              context,
              projection,
              details.globalPosition,
            ),
            onPanEnd: (_) => _endMoveMark(),
            onPanCancel: _endMoveMark,
            child: const SizedBox.expand(),
          ),
        ),
      ),
    );
  }

  bool _consumeDoubleClick(QuickMark mark, Duration time, Offset position) {
    final lastMarkId = _lastPointerDownMarkId;
    final lastTime = _lastPointerDownTime;
    final lastPosition = _lastPointerDownPosition;
    _lastPointerDownMarkId = mark.id;
    _lastPointerDownTime = time;
    _lastPointerDownPosition = position;
    if (lastMarkId != mark.id || lastTime == null || lastPosition == null) {
      return false;
    }
    final elapsed = time - lastTime;
    final isDoubleClick =
        elapsed <= const Duration(milliseconds: 420) &&
        (position - lastPosition).distance <= 8.0;
    if (isDoubleClick) {
      _lastPointerDownMarkId = null;
      _lastPointerDownTime = null;
      _lastPointerDownPosition = null;
    }
    return isDoubleClick;
  }

  List<Widget> _buildArrowHitTargets(
    BuildContext context,
    ViewportLayoutProjection projection,
    QuickMark mark,
    Rect viewportRect,
    Rect clipRect,
    List<Rect> excludedRects,
  ) {
    final start = _sourcePointToViewportRect(
      viewportRect,
      mark.sourceRect,
      mark.effectiveSourceStart,
    );
    final end = _sourcePointToViewportRect(
      viewportRect,
      mark.sourceRect,
      mark.effectiveSourceEnd,
    );
    final segments = _arrowSegments(start, end, mark.strokeWidth);
    if (segments.isEmpty) return const [];

    final hitSize = math.max(_borderHitWidth, mark.strokeWidth + 10.0);
    final hitClip = clipRect.inflate(hitSize / 2);
    final targets = <Widget>[];
    for (final segment in segments) {
      final delta = segment.end - segment.start;
      final length = delta.distance;
      if (length <= 0.1) continue;
      final steps = math.max(1, (length / (hitSize * 0.55)).ceil());
      for (var i = 0; i <= steps; i += 1) {
        final center = Offset.lerp(segment.start, segment.end, i / steps)!;
        if (!hitClip.contains(center)) continue;
        final targetRect = Rect.fromCenter(
          center: center,
          width: hitSize,
          height: hitSize,
        );
        if (excludedRects.any((excluded) => excluded.overlaps(targetRect))) {
          continue;
        }
        targets.add(
          _buildBorderHitTarget(context, projection, mark, targetRect),
        );
      }
    }
    return targets;
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

  List<Widget> _buildSelectedMarkControls(
    BuildContext context,
    ViewportLayoutProjection projection,
    QuickMark mark,
    Rect rect,
    Rect clipRect,
    Size viewportSize,
  ) {
    if (mark.shape == QuickMarkShape.arrow) {
      return _buildSelectedArrowControls(
        context,
        projection,
        mark,
        rect,
        clipRect,
        viewportSize,
      );
    }

    final handles = <Widget>[];
    for (final handle in _MarkHandle.values) {
      final center = _handleCenter(rect, handle);
      if (!clipRect.contains(center)) continue;
      handles.add(
        Positioned(
          key: ValueKey('quick-mark-rect-${mark.id}-${handle.name}'),
          left: center.dx - _handleHitSize / 2,
          top: center.dy - _handleHitSize / 2,
          width: _handleHitSize,
          height: _handleHitSize,
          child: MouseRegion(
            cursor: _cursorForHandle(handle),
            child: Listener(
              onPointerDown: (event) {
                if (_consumeDoubleClick(
                  mark,
                  event.timeStamp,
                  event.position,
                )) {
                  _startEditingText(mark);
                }
              },
              child: GestureDetector(
                behavior: HitTestBehavior.opaque,
                onPanStart: (details) => _startResizeMarkHandle(
                  context,
                  projection,
                  mark,
                  handle,
                  details.globalPosition,
                ),
                onPanUpdate: (details) => _resizeMarkFromGlobalPosition(
                  context,
                  projection,
                  handle,
                  details.globalPosition,
                ),
                onPanEnd: (_) => _endResizeMarkHandle(mark, handle),
                onPanCancel: () => _cancelResizeMarkHandle(mark, handle),
                child: const Center(child: _QuickMarkHandle()),
              ),
            ),
          ),
        ),
      );
    }

    final visibleRect = rect.intersect(clipRect);
    return [
      ...handles,
      if (!visibleRect.isEmpty)
        _buildStylePanel(mark, visibleRect, viewportSize),
    ];
  }

  List<Widget> _buildSelectedArrowControls(
    BuildContext context,
    ViewportLayoutProjection projection,
    QuickMark mark,
    Rect rect,
    Rect clipRect,
    Size viewportSize,
  ) {
    final handles = <Widget>[];
    for (final endpoint in _ArrowEndpoint.values) {
      final center = _arrowEndpointCenter(mark, rect, endpoint);
      if (!clipRect.contains(center)) continue;
      handles.add(
        Positioned(
          key: ValueKey('quick-mark-arrow-${mark.id}-${endpoint.name}'),
          left: center.dx - _handleHitSize / 2,
          top: center.dy - _handleHitSize / 2,
          width: _handleHitSize,
          height: _handleHitSize,
          child: MouseRegion(
            cursor: SystemMouseCursors.grab,
            child: Listener(
              onPointerDown: (event) {
                if (_consumeDoubleClick(
                  mark,
                  event.timeStamp,
                  event.position,
                )) {
                  _startEditingText(mark);
                }
              },
              child: GestureDetector(
                behavior: HitTestBehavior.opaque,
                onPanStart: (details) => _startResizeArrowEndpoint(
                  context,
                  projection,
                  mark,
                  endpoint,
                  details.globalPosition,
                ),
                onPanUpdate: (details) =>
                    _resizeArrowEndpointFromGlobalPosition(
                      context,
                      projection,
                      endpoint,
                      details.globalPosition,
                    ),
                onPanEnd: (_) => _endResizeArrowEndpoint(mark, endpoint),
                onPanCancel: () => _cancelResizeArrowEndpoint(mark, endpoint),
                child: const Center(child: _QuickMarkHandle()),
              ),
            ),
          ),
        ),
      );
    }

    final visibleRect = rect.intersect(clipRect);
    return [
      ...handles,
      if (!visibleRect.isEmpty)
        _buildStylePanel(mark, visibleRect, viewportSize),
    ];
  }

  Widget _buildTextEditor(
    BuildContext context,
    QuickMark mark,
    Rect rect,
    Rect clipRect,
  ) {
    final layout = _textLayoutForMark(
      mark,
      rect,
      clipRect,
      text: _textController.text,
    );
    if (layout == null) return const SizedBox.shrink();
    return Positioned.fromRect(
      rect: layout.rect,
      child: ClipRect(
        child: Shortcuts(
          shortcuts: const {
            SingleActivator(LogicalKeyboardKey.escape): _CancelTextEditIntent(),
            SingleActivator(LogicalKeyboardKey.enter): _CommitTextEditIntent(),
            SingleActivator(LogicalKeyboardKey.numpadEnter):
                _CommitTextEditIntent(),
          },
          child: Actions(
            actions: {
              _CancelTextEditIntent: CallbackAction<_CancelTextEditIntent>(
                onInvoke: (_) {
                  _stopEditingText(commit: true);
                  return null;
                },
              ),
              _CommitTextEditIntent: CallbackAction<_CommitTextEditIntent>(
                onInvoke: (_) {
                  _stopEditingText(commit: true);
                  _notifyInteraction();
                  widget.onSelectedMarkChanged?.call(null);
                  return null;
                },
              ),
            },
            child: EditableText(
              controller: _textController,
              focusNode: _textFocusNode,
              autofocus: true,
              keyboardType: TextInputType.multiline,
              maxLines: null,
              cursorColor: mark.color,
              backgroundCursorColor: Colors.transparent,
              style: layout.style,
              textDirection: TextDirection.ltr,
              onChanged: (value) {
                final current = _markById(mark.id);
                if (current == null || current.text == value) return;
                setState(() {});
                _notifyInteraction();
                widget.onMarkChanged?.call(current.copyWith(text: value));
              },
              onTapOutside: (_) => _textFocusNode.unfocus(),
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildStylePanel(QuickMark mark, Rect markRect, Size viewportSize) {
    final colorScheme = Theme.of(context).colorScheme;
    final belowTop = markRect.bottom + _panelGap;
    final aboveTop = markRect.top - _panelGap - _panelHeight;
    final panelTop = belowTop + _panelHeight <= viewportSize.height - 8.0
        ? belowTop
        : math.max(8.0, aboveTop);
    return Positioned(
      left: 0,
      right: 0,
      top: panelTop,
      height: _panelHeight,
      child: CustomSingleChildLayout(
        delegate: _QuickMarkPanelLayoutDelegate(anchorX: markRect.center.dx),
        child: Material(
          color: colorScheme.surfaceContainerHighest.withValues(
            alpha: colorScheme.brightness == Brightness.dark ? 0.88 : 0.94,
          ),
          elevation: 10,
          shadowColor: colorScheme.shadow.withValues(alpha: 0.28),
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
          clipBehavior: Clip.antiAlias,
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 4),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                _syncButton(mark),
                _focusButton(mark),
                const SizedBox(width: 4),
                const _QuickMarkPanelSeparator(),
                const SizedBox(width: 4),
                _textButton(mark),
                _boldButton(mark),
                _fontSizeCombo(mark),
                const SizedBox(width: 4),
                const _QuickMarkPanelSeparator(),
                const SizedBox(width: 4),
                _shapeButton(mark, QuickMarkShape.rectangle, Icons.crop_square),
                _shapeButton(mark, QuickMarkShape.arrow, Icons.arrow_forward),
                const SizedBox(width: 4),
                const _QuickMarkPanelSeparator(),
                const SizedBox(width: 4),
                _colorCombo(mark),
                const SizedBox(width: 4),
                const _QuickMarkPanelSeparator(),
                const SizedBox(width: 4),
                _strokeCombo(mark),
                const SizedBox(width: 4),
                const _QuickMarkPanelSeparator(),
                const SizedBox(width: 4),
                _deleteButton(mark),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _syncButton(QuickMark mark) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    final selected = mark.syncAcrossTracks;
    return Tooltip(
      message: l.quickMarkSync,
      child: SizedBox(
        width: _panelButtonSize,
        height: _panelButtonSize,
        child: IconButton(
          visualDensity: VisualDensity.compact,
          padding: EdgeInsets.zero,
          constraints: const BoxConstraints.tightFor(
            width: _panelButtonSize,
            height: _panelButtonSize,
          ),
          iconSize: 18,
          color: selected ? colorScheme.primary : colorScheme.onSurfaceVariant,
          style: _quickMarkToggleButtonStyle(
            selected,
            colorScheme,
            const Size.square(_panelButtonSize),
          ),
          onPressed: () {
            _notifyInteraction();
            widget.onMarkChanged?.call(
              mark.copyWith(syncAcrossTracks: !mark.syncAcrossTracks),
            );
          },
          icon: const Icon(Icons.sync),
        ),
      ),
    );
  }

  Widget _focusButton(QuickMark mark) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    return Tooltip(
      message: l.quickMarkFocus,
      child: SizedBox(
        width: _panelButtonSize,
        height: _panelButtonSize,
        child: IconButton(
          visualDensity: VisualDensity.compact,
          padding: EdgeInsets.zero,
          constraints: const BoxConstraints.tightFor(
            width: _panelButtonSize,
            height: _panelButtonSize,
          ),
          iconSize: 18,
          color: colorScheme.onSurfaceVariant,
          style: _quickMarkToggleButtonStyle(
            false,
            colorScheme,
            const Size.square(_panelButtonSize),
          ),
          onPressed: () {
            _notifyInteraction();
            widget.onMarkFocus?.call(mark.id);
          },
          icon: const Icon(Icons.center_focus_strong),
        ),
      ),
    );
  }

  Widget _textButton(QuickMark mark) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    final selected = _editingTextMarkId == mark.id;
    return Tooltip(
      message: l.quickMarkText,
      child: SizedBox(
        width: _panelButtonSize,
        height: _panelButtonSize,
        child: IconButton(
          visualDensity: VisualDensity.compact,
          padding: EdgeInsets.zero,
          constraints: const BoxConstraints.tightFor(
            width: _panelButtonSize,
            height: _panelButtonSize,
          ),
          iconSize: 18,
          color: selected ? colorScheme.primary : colorScheme.onSurfaceVariant,
          style: _quickMarkToggleButtonStyle(
            selected,
            colorScheme,
            const Size.square(_panelButtonSize),
          ),
          onPressed: () => _startEditingText(mark),
          icon: const Icon(Icons.title),
        ),
      ),
    );
  }

  Widget _boldButton(QuickMark mark) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    final selected = mark.textBold;
    return Tooltip(
      message: l.quickMarkBold,
      child: SizedBox(
        width: _panelButtonSize,
        height: _panelButtonSize,
        child: IconButton(
          visualDensity: VisualDensity.compact,
          padding: EdgeInsets.zero,
          constraints: const BoxConstraints.tightFor(
            width: _panelButtonSize,
            height: _panelButtonSize,
          ),
          iconSize: 18,
          color: selected ? colorScheme.primary : colorScheme.onSurfaceVariant,
          style: _quickMarkToggleButtonStyle(
            selected,
            colorScheme,
            const Size.square(_panelButtonSize),
          ),
          onPressed: () {
            _notifyInteraction();
            widget.onMarkChanged?.call(mark.copyWith(textBold: !selected));
          },
          icon: const Icon(Icons.format_bold),
        ),
      ),
    );
  }

  Widget _fontSizeCombo(QuickMark mark) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    final theme = Theme.of(context);
    return Tooltip(
      message: l.quickMarkTextSize,
      child: AppMenuCombo<double>(
        width: 48,
        height: _panelButtonSize,
        value: mark.textFontSize,
        items: _fontSizes,
        labelFor: (size) => size.round().toString(),
        onChanged: (size) {
          _notifyInteraction();
          widget.onMarkChanged?.call(mark.copyWith(textFontSize: size));
        },
        textStyle: theme.textTheme.bodySmall?.copyWith(
          color: colorScheme.onSurfaceVariant,
          fontWeight: FontWeight.w600,
        ),
        menuTextStyle: theme.textTheme.bodySmall,
        maxMenuWidth: 92,
        itemHeight: 30,
        buttonPadding: const EdgeInsets.only(left: 6, right: 2),
        itemPadding: const EdgeInsets.only(left: 10, right: 14),
        borderRadius: BorderRadius.circular(4),
        backgroundColor: Colors.transparent,
        foregroundColor: colorScheme.onSurfaceVariant,
        iconSize: 16,
      ),
    );
  }

  Widget _deleteButton(QuickMark mark) {
    return _QuickMarkDeleteButton(
      onPressed: widget.onMarkDeleted == null
          ? null
          : () {
              _notifyInteraction();
              widget.onMarkDeleted?.call(mark.id);
            },
    );
  }

  Widget _shapeButton(QuickMark mark, QuickMarkShape shape, IconData icon) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    final selected = mark.shape == shape;
    return Tooltip(
      message: shape == QuickMarkShape.rectangle
          ? l.quickMarkRectangle
          : l.quickMarkArrow,
      child: SizedBox(
        width: _panelButtonSize,
        height: _panelButtonSize,
        child: IconButton(
          visualDensity: VisualDensity.compact,
          padding: EdgeInsets.zero,
          constraints: const BoxConstraints.tightFor(
            width: _panelButtonSize,
            height: _panelButtonSize,
          ),
          iconSize: 18,
          color: selected ? colorScheme.primary : colorScheme.onSurfaceVariant,
          style: _quickMarkToggleButtonStyle(
            selected,
            colorScheme,
            const Size.square(_panelButtonSize),
          ),
          onPressed: () {
            _notifyInteraction();
            widget.onMarkChanged?.call(mark.copyWith(shape: shape));
          },
          icon: Icon(icon),
        ),
      ),
    );
  }

  Widget _colorCombo(QuickMark mark) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    return Tooltip(
      message: l.quickMarkColor,
      child: AppMenuCombo<Color>(
        width: 46,
        height: _panelButtonSize,
        value: mark.color,
        items: _colors,
        labelFor: (color) => _colorLabel(l, color),
        onChanged: (color) {
          _notifyInteraction();
          widget.onMarkChanged?.call(mark.copyWith(color: color));
        },
        minMenuWidth: 136,
        maxMenuWidth: 180,
        itemHeight: 30,
        buttonPadding: const EdgeInsets.only(left: 6, right: 2),
        itemPadding: const EdgeInsets.only(left: 10, right: 12),
        borderRadius: BorderRadius.circular(4),
        backgroundColor: Colors.transparent,
        foregroundColor: colorScheme.onSurfaceVariant,
        iconSize: 16,
        showSelectedCheck: false,
        buttonBuilder: (context, color, open) => Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            _QuickMarkColorSwatch(
              color: color,
              selected: true,
              colorScheme: colorScheme,
            ),
            const SizedBox(width: 2),
            AppMenuComboArrow(
              open: open,
              size: 16,
              color: colorScheme.onSurfaceVariant,
            ),
          ],
        ),
        itemBuilder: (context, color, label, selected) => _QuickMarkColorOption(
          color: color,
          label: label,
          selected: selected,
        ),
      ),
    );
  }

  Widget _strokeCombo(QuickMark mark) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    final strokeLabel = l.quickMarkStrokeWidth(mark.strokeWidth.round());
    return Tooltip(
      message: l.quickMarkStroke,
      child: AppMenuCombo<double>(
        width: 80,
        height: _panelButtonSize,
        value: mark.strokeWidth,
        items: _strokeWidths,
        labelFor: (width) => l.quickMarkStrokeWidth(width.round()),
        onChanged: (width) {
          _notifyInteraction();
          widget.onMarkChanged?.call(mark.copyWith(strokeWidth: width));
        },
        minMenuWidth: 132,
        maxMenuWidth: 160,
        itemHeight: 30,
        buttonPadding: const EdgeInsets.only(left: 6, right: 2),
        itemPadding: const EdgeInsets.only(left: 10, right: 12),
        borderRadius: BorderRadius.circular(4),
        backgroundColor: Colors.transparent,
        foregroundColor: colorScheme.onSurfaceVariant,
        iconSize: 16,
        showSelectedCheck: false,
        buttonBuilder: (context, width, open) => Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            SizedBox(
              width: 28,
              child: Text(
                strokeLabel,
                style: Theme.of(context).textTheme.labelSmall?.copyWith(
                  color: colorScheme.onSurfaceVariant,
                  fontWeight: FontWeight.w600,
                ),
                overflow: TextOverflow.clip,
                maxLines: 1,
              ),
            ),
            const SizedBox(width: 3),
            _QuickMarkStrokePreview(width: width, previewWidth: 18),
            AppMenuComboArrow(
              open: open,
              size: 14,
              color: colorScheme.onSurfaceVariant,
            ),
          ],
        ),
        itemBuilder: (context, width, label, selected) =>
            _QuickMarkStrokeOption(
              label: label,
              width: width,
              selected: selected,
            ),
      ),
    );
  }

  String _colorLabel(AppLocalizations l, Color color) {
    if (color == const Color(0xFFFF3B30)) return l.quickMarkColorRed;
    if (color == const Color(0xFFFF9500)) return l.quickMarkColorOrange;
    if (color == const Color(0xFFFFCC00)) return l.quickMarkColorYellow;
    if (color == const Color(0xFF34C759)) return l.quickMarkColorGreen;
    if (color == const Color(0xFF00C7BE)) return l.quickMarkColorCyan;
    if (color == const Color(0xFF0A84FF)) return l.quickMarkColorBlue;
    if (color == const Color(0xFFBF5AF2)) return l.quickMarkColorPurple;
    if (color == const Color(0xFFFFFFFF)) return l.quickMarkColorWhite;
    if (color == const Color(0xFF000000)) return l.quickMarkColorBlack;
    return l.quickMarkColor;
  }

  void _resizeMarkFromGlobalPosition(
    BuildContext context,
    ViewportLayoutProjection projection,
    _MarkHandle handle,
    Offset globalPosition,
  ) {
    final start = _resizeStartMark;
    if (start == null) return;
    final box = context.findRenderObject() as RenderBox?;
    if (box == null) return;
    final local = box.globalToLocal(globalPosition);
    final sourceUv = projection.sourceUvForTrackPhysical(
      start.fileId,
      local * widget.devicePixelRatio,
      clipToVisibleRegion: true,
    );
    if (sourceUv == null) return;
    final next = _markForHandleDrag(start, handle, sourceUv);
    _notifyInteraction();
    widget.onMarkChanged?.call(next);
  }

  void _startResizeMarkHandle(
    BuildContext context,
    ViewportLayoutProjection projection,
    QuickMark mark,
    _MarkHandle handle,
    Offset globalPosition,
  ) {
    _notifyInteraction();
    _resizeStartMark = mark;
  }

  void _endResizeMarkHandle(QuickMark mark, _MarkHandle handle) {
    _resizeStartMark = null;
  }

  void _cancelResizeMarkHandle(QuickMark mark, _MarkHandle handle) {
    _resizeStartMark = null;
  }

  void _resizeArrowEndpointFromGlobalPosition(
    BuildContext context,
    ViewportLayoutProjection projection,
    _ArrowEndpoint endpoint,
    Offset globalPosition,
  ) {
    final start = _resizeStartMark;
    if (start == null) return;
    final activeEndpoint = _activeArrowEndpoint ?? endpoint;
    final local = _localFromGlobal(context, globalPosition);
    final sourceUv = local == null
        ? null
        : projection.sourceUvForTrackPhysical(
            start.fileId,
            local * widget.devicePixelRatio,
            clipToVisibleRegion: true,
          );
    if (sourceUv == null) return;
    final sourceStart = activeEndpoint == _ArrowEndpoint.start
        ? sourceUv
        : start.effectiveSourceStart;
    final sourceEnd = activeEndpoint == _ArrowEndpoint.end
        ? sourceUv
        : start.effectiveSourceEnd;
    final next = start.copyWith(
      sourceRect: Rect.fromPoints(sourceStart, sourceEnd),
      sourceStart: sourceStart,
      sourceEnd: sourceEnd,
    );
    _notifyInteraction();
    widget.onMarkChanged?.call(next);
  }

  void _startResizeArrowEndpoint(
    BuildContext context,
    ViewportLayoutProjection projection,
    QuickMark mark,
    _ArrowEndpoint endpoint,
    Offset globalPosition,
  ) {
    _notifyInteraction();
    _resizeStartMark = mark;
    _activeArrowEndpoint = endpoint;
  }

  void _endResizeArrowEndpoint(QuickMark mark, _ArrowEndpoint endpoint) {
    _resizeStartMark = null;
    _activeArrowEndpoint = null;
  }

  void _cancelResizeArrowEndpoint(QuickMark mark, _ArrowEndpoint endpoint) {
    _resizeStartMark = null;
    _activeArrowEndpoint = null;
  }

  QuickMark _markForHandleDrag(
    QuickMark mark,
    _MarkHandle handle,
    Offset sourceUv,
  ) {
    final raw = _rawRectForHandleDrag(mark.sourceRect, handle, sourceUv);
    final rect = _normalizedRectFromRaw(raw);
    final sourceStart = _mapPointFromRectToRaw(
      mark.sourceRect,
      raw,
      mark.effectiveSourceStart,
    );
    final sourceEnd = _mapPointFromRectToRaw(
      mark.sourceRect,
      raw,
      mark.effectiveSourceEnd,
    );
    return mark.copyWith(
      sourceRect: rect,
      sourceStart: sourceStart,
      sourceEnd: sourceEnd,
    );
  }

  ({double left, double top, double right, double bottom})
  _rawRectForHandleDrag(Rect sourceRect, _MarkHandle handle, Offset sourceUv) {
    var left = sourceRect.left;
    var top = sourceRect.top;
    var right = sourceRect.right;
    var bottom = sourceRect.bottom;
    switch (handle) {
      case _MarkHandle.topLeft:
        left = sourceUv.dx;
        top = sourceUv.dy;
      case _MarkHandle.top:
        top = sourceUv.dy;
      case _MarkHandle.topRight:
        right = sourceUv.dx;
        top = sourceUv.dy;
      case _MarkHandle.right:
        right = sourceUv.dx;
      case _MarkHandle.bottomRight:
        right = sourceUv.dx;
        bottom = sourceUv.dy;
      case _MarkHandle.bottom:
        bottom = sourceUv.dy;
      case _MarkHandle.bottomLeft:
        left = sourceUv.dx;
        bottom = sourceUv.dy;
      case _MarkHandle.left:
        left = sourceUv.dx;
    }
    return (left: left, top: top, right: right, bottom: bottom);
  }

  Rect _normalizedRectFromRaw(
    ({double left, double top, double right, double bottom}) raw,
  ) {
    return Rect.fromLTRB(
      math.min(raw.left, raw.right).clamp(0.0, 1.0),
      math.min(raw.top, raw.bottom).clamp(0.0, 1.0),
      math.max(raw.left, raw.right).clamp(0.0, 1.0),
      math.max(raw.top, raw.bottom).clamp(0.0, 1.0),
    );
  }

  Offset _mapPointFromRectToRaw(
    Rect from,
    ({double left, double top, double right, double bottom}) raw,
    Offset point,
  ) {
    final tx = from.width.abs() <= 1e-6
        ? 0.5
        : (point.dx - from.left) / from.width;
    final ty = from.height.abs() <= 1e-6
        ? 0.5
        : (point.dy - from.top) / from.height;
    return Offset(
      (raw.left + (raw.right - raw.left) * tx).clamp(0.0, 1.0),
      (raw.top + (raw.bottom - raw.top) * ty).clamp(0.0, 1.0),
    );
  }

  void _startMoveMark(
    BuildContext context,
    ViewportLayoutProjection projection,
    QuickMark mark,
    Offset globalPosition,
  ) {
    _notifyInteraction();
    widget.onSelectedMarkChanged?.call(mark.id);
    _moveStartMark = mark;
    _moveStartSourceUv = _sourceUvFromGlobalPosition(
      context,
      projection,
      mark.fileId,
      globalPosition,
    );
  }

  void _moveMarkFromGlobalPosition(
    BuildContext context,
    ViewportLayoutProjection projection,
    Offset globalPosition,
  ) {
    final mark = _moveStartMark;
    final startUv = _moveStartSourceUv;
    if (mark == null || startUv == null) return;
    final currentUv = _sourceUvFromGlobalPosition(
      context,
      projection,
      mark.fileId,
      globalPosition,
    );
    if (currentUv == null) return;

    final requestedDelta = currentUv - startUv;
    final delta = Offset(
      requestedDelta.dx.clamp(
        -mark.sourceRect.left,
        1.0 - mark.sourceRect.right,
      ),
      requestedDelta.dy.clamp(
        -mark.sourceRect.top,
        1.0 - mark.sourceRect.bottom,
      ),
    );
    final next = mark.copyWith(
      sourceRect: mark.sourceRect.shift(delta),
      sourceStart: mark.effectiveSourceStart + delta,
      sourceEnd: mark.effectiveSourceEnd + delta,
    );
    _notifyInteraction();
    widget.onMarkChanged?.call(next);
  }

  void _endMoveMark() {
    _moveStartMark = null;
    _moveStartSourceUv = null;
  }

  Offset? _sourceUvFromGlobalPosition(
    BuildContext context,
    ViewportLayoutProjection projection,
    int fileId,
    Offset globalPosition,
  ) {
    final box = context.findRenderObject() as RenderBox?;
    if (box == null) return null;
    final local = box.globalToLocal(globalPosition);
    return projection.sourceUvForTrackPhysical(
      fileId,
      local * widget.devicePixelRatio,
      clipToVisibleRegion: true,
    );
  }

  Offset? _localFromGlobal(BuildContext context, Offset globalPosition) {
    final box = context.findRenderObject() as RenderBox?;
    if (box == null) return null;
    return box.globalToLocal(globalPosition);
  }

  _LogicalProjectedMark? _logicalViewportProjection(
    ViewportLayoutProjection projection,
    QuickMark mark,
  ) {
    return _logicalViewportProjectionForFile(projection, mark, mark.fileId);
  }

  _LogicalProjectedMark? _logicalViewportProjectionForFile(
    ViewportLayoutProjection projection,
    QuickMark mark,
    int fileId,
  ) {
    if (!projection.isValid || widget.devicePixelRatio <= 0) return null;
    final projected = projection.viewportProjectionForSourceRect(
      fileId,
      mark.sourceRect,
    );
    if (projected == null || !projected.isVisible) return null;
    return _LogicalProjectedMark(
      viewportRect: _logicalRect(projected.viewportRect),
      clipRect: _logicalRect(projected.clipRect),
    );
  }

  Rect _logicalRect(Rect physicalRect) {
    return Rect.fromLTRB(
      physicalRect.left / widget.devicePixelRatio,
      physicalRect.top / widget.devicePixelRatio,
      physicalRect.right / widget.devicePixelRatio,
      physicalRect.bottom / widget.devicePixelRatio,
    );
  }

  Offset _handleCenter(Rect rect, _MarkHandle handle) {
    switch (handle) {
      case _MarkHandle.topLeft:
        return rect.topLeft;
      case _MarkHandle.top:
        return Offset(rect.center.dx, rect.top);
      case _MarkHandle.topRight:
        return rect.topRight;
      case _MarkHandle.right:
        return Offset(rect.right, rect.center.dy);
      case _MarkHandle.bottomRight:
        return rect.bottomRight;
      case _MarkHandle.bottom:
        return Offset(rect.center.dx, rect.bottom);
      case _MarkHandle.bottomLeft:
        return rect.bottomLeft;
      case _MarkHandle.left:
        return Offset(rect.left, rect.center.dy);
    }
  }

  Offset _arrowEndpointCenter(
    QuickMark mark,
    Rect viewportRect,
    _ArrowEndpoint endpoint,
  ) {
    return _sourcePointToViewportRect(
      viewportRect,
      mark.sourceRect,
      endpoint == _ArrowEndpoint.start
          ? mark.effectiveSourceStart
          : mark.effectiveSourceEnd,
    );
  }

  List<Rect> _arrowEndpointHitRects(QuickMark mark, Rect viewportRect) {
    return [
      for (final endpoint in _ArrowEndpoint.values)
        Rect.fromCenter(
          center: _arrowEndpointCenter(mark, viewportRect, endpoint),
          width: _handleHitSize + _borderHitWidth,
          height: _handleHitSize + _borderHitWidth,
        ),
    ];
  }

  _QuickMarkTextLayout? _textLayoutForMark(
    QuickMark mark,
    Rect rect,
    Rect clipRect, {
    required String text,
  }) {
    return _quickMarkTextLayout(mark, rect, clipRect, text: text);
  }

  Offset _sourcePointToViewportRect(
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

  MouseCursor _cursorForHandle(_MarkHandle handle) {
    switch (handle) {
      case _MarkHandle.topLeft:
      case _MarkHandle.bottomRight:
        return const _NativeDiagonalResizeCursor.upLeftDownRight();
      case _MarkHandle.topRight:
      case _MarkHandle.bottomLeft:
        return const _NativeDiagonalResizeCursor.upRightDownLeft();
      case _MarkHandle.top:
      case _MarkHandle.bottom:
        return SystemMouseCursors.resizeUpDown;
      case _MarkHandle.left:
      case _MarkHandle.right:
        return SystemMouseCursors.resizeLeftRight;
    }
  }
}

class _QuickMarkPainter extends CustomPainter {
  final ViewportLayoutProjection projection;
  final List<DisplayTrackGeometry> tracks;
  final List<QuickMark> marks;
  final QuickMark? draft;
  final int? selectedMarkId;
  final int? editingTextMarkId;
  final double devicePixelRatio;

  _QuickMarkPainter({
    required this.projection,
    required this.tracks,
    required this.marks,
    required this.draft,
    required this.selectedMarkId,
    required this.editingTextMarkId,
    required this.devicePixelRatio,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (!projection.isValid || devicePixelRatio <= 0) return;
    for (final mark in marks) {
      if (mark.syncAcrossTracks && tracks.length > 1) {
        for (final track in tracks) {
          if (track.fileId == mark.fileId) continue;
          _drawMark(
            canvas,
            mark.copyWith(
              anchor: mark.anchor.copyWith(fileId: track.fileId),
              color: _syncedMarkColor(mark.color),
              text: '',
            ),
            selected: false,
            draft: false,
          );
        }
      }
      _drawMark(
        canvas,
        mark,
        selected: mark.id == selectedMarkId,
        draft: false,
      );
    }
    final activeDraft = draft;
    if (activeDraft != null) {
      _drawMark(canvas, activeDraft, selected: false, draft: true);
    }
  }

  void _drawMark(
    Canvas canvas,
    QuickMark mark, {
    required bool selected,
    required bool draft,
  }) {
    final projected = projection.viewportProjectionForSourceRect(
      mark.fileId,
      mark.sourceRect,
    );
    if (projected == null || !projected.isVisible) return;
    final rect = _logicalRect(projected.viewportRect);
    final clipRect = _logicalRect(projected.clipRect);
    if (clipRect.isEmpty) return;
    if (!rect.inflate(mark.strokeWidth + 6).overlaps(clipRect)) return;

    canvas.save();
    canvas.clipRect(clipRect);
    _drawMarkInClip(
      canvas,
      mark,
      rect,
      clipRect,
      selected: selected,
      draft: draft,
    );
    canvas.restore();
  }

  Rect _logicalRect(Rect physicalRect) {
    return Rect.fromLTRB(
      physicalRect.left / devicePixelRatio,
      physicalRect.top / devicePixelRatio,
      physicalRect.right / devicePixelRatio,
      physicalRect.bottom / devicePixelRatio,
    );
  }

  void _drawMarkInClip(
    Canvas canvas,
    QuickMark mark,
    Rect rect,
    Rect clipRect, {
    required bool selected,
    required bool draft,
  }) {
    if (rect.width <= 0 || rect.height <= 0) return;

    final strokeWidth = draft ? mark.strokeWidth + 0.5 : mark.strokeWidth;
    final hasVisibleText = !draft && mark.text.trim().isNotEmpty;
    final textLayout = hasVisibleText && mark.shape == QuickMarkShape.arrow
        ? _textLayoutForMark(mark, rect, clipRect)
        : null;
    if (selected) {
      _drawShapeWithTextExclusion(
        canvas,
        mark,
        rect,
        clipRect,
        textLayout,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = strokeWidth + 4
          ..strokeCap = StrokeCap.round
          ..strokeJoin = StrokeJoin.round
          ..color = const Color(0xCC000000),
      );
      _drawShapeWithTextExclusion(
        canvas,
        mark,
        rect,
        clipRect,
        textLayout,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = strokeWidth + 2
          ..strokeCap = StrokeCap.round
          ..strokeJoin = StrokeJoin.round
          ..color = const Color(0xFFFFFFFF),
      );
    }
    _drawShapeWithTextExclusion(
      canvas,
      mark,
      rect,
      clipRect,
      textLayout,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = strokeWidth
        ..strokeCap = StrokeCap.round
        ..strokeJoin = StrokeJoin.round
        ..color = draft ? mark.color.withValues(alpha: 0.82) : mark.color,
    );
    if (hasVisibleText && mark.id != editingTextMarkId) {
      _drawText(canvas, mark, rect, clipRect);
    }
  }

  Color _syncedMarkColor(Color source) {
    final hsl = HSLColor.fromColor(source);
    return hsl
        .withSaturation((hsl.saturation * 0.45).clamp(0.0, 1.0))
        .withLightness((hsl.lightness * 0.72).clamp(0.0, 1.0))
        .toColor()
        .withValues(alpha: 0.74);
  }

  void _drawShapeWithTextExclusion(
    Canvas canvas,
    QuickMark mark,
    Rect rect,
    Rect clipRect,
    _QuickMarkTextLayout? textLayout,
    Paint paint,
  ) {
    if (textLayout == null || textLayout.rect.isEmpty) {
      _drawShape(canvas, mark, rect, paint);
      return;
    }
    canvas.save();
    final exclusion = textLayout.rect.inflate(2);
    canvas.clipPath(
      Path()
        ..fillType = PathFillType.evenOdd
        ..addRect(clipRect)
        ..addRRect(
          RRect.fromRectAndRadius(exclusion, const Radius.circular(2)),
        ),
    );
    _drawShape(canvas, mark, rect, paint);
    canvas.restore();
  }

  void _drawShape(Canvas canvas, QuickMark mark, Rect rect, Paint paint) {
    switch (mark.shape) {
      case QuickMarkShape.rectangle:
        canvas.drawRect(rect, paint);
      case QuickMarkShape.arrow:
        canvas.drawPath(
          _arrowPath(
            _sourcePointToViewportRect(
              rect,
              mark.sourceRect,
              mark.effectiveSourceStart,
            ),
            _sourcePointToViewportRect(
              rect,
              mark.sourceRect,
              mark.effectiveSourceEnd,
            ),
            mark.strokeWidth,
          ),
          paint,
        );
    }
  }

  Offset _sourcePointToViewportRect(
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

  Path _arrowPath(Offset start, Offset end, double baseStrokeWidth) {
    final path = Path()
      ..moveTo(start.dx, start.dy)
      ..lineTo(end.dx, end.dy);
    final delta = end - start;
    if (delta.distance <= 0.1) return path;
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
    return path
      ..moveTo(end.dx, end.dy)
      ..lineTo(left.dx, left.dy)
      ..moveTo(end.dx, end.dy)
      ..lineTo(right.dx, right.dy);
  }

  void _drawText(Canvas canvas, QuickMark mark, Rect rect, Rect clipRect) {
    final text = mark.text.trim();
    if (text.isEmpty) return;
    final layout = _textLayoutForMark(mark, rect, clipRect);
    if (layout == null) return;
    final painter = TextPainter(
      text: TextSpan(text: text, style: layout.style),
      maxLines: 5,
      ellipsis: '…',
      textDirection: TextDirection.ltr,
    )..layout(maxWidth: layout.textMaxWidth);
    painter.paint(canvas, layout.rect.topLeft);
  }

  _QuickMarkTextLayout? _textLayoutForMark(
    QuickMark mark,
    Rect rect,
    Rect clipRect,
  ) {
    return _quickMarkTextLayout(mark, rect, clipRect, text: mark.text);
  }

  @override
  bool shouldRepaint(covariant _QuickMarkPainter oldDelegate) {
    return oldDelegate.projection != projection ||
        oldDelegate.tracks != tracks ||
        oldDelegate.marks != marks ||
        oldDelegate.draft != draft ||
        oldDelegate.selectedMarkId != selectedMarkId ||
        oldDelegate.editingTextMarkId != editingTextMarkId ||
        oldDelegate.devicePixelRatio != devicePixelRatio;
  }
}

class _NativeDiagonalResizeCursor extends MouseCursor {
  final String kind;

  const _NativeDiagonalResizeCursor.upLeftDownRight()
    : kind = 'upLeftDownRight';

  const _NativeDiagonalResizeCursor.upRightDownLeft()
    : kind = 'upRightDownLeft';

  @override
  MouseCursorSession createSession(int device) =>
      _NativeDiagonalResizeCursorSession(this, device);

  @override
  String get debugDescription => 'native diagonal resize $kind';
}

class _NativeDiagonalResizeCursorSession extends MouseCursorSession {
  static const MethodChannel _channel = MethodChannel(
    'void_player/quick_mark_cursor',
  );

  _NativeDiagonalResizeCursorSession(
    _NativeDiagonalResizeCursor super.cursor,
    super.device,
  );

  @override
  _NativeDiagonalResizeCursor get cursor =>
      super.cursor as _NativeDiagonalResizeCursor;

  @override
  Future<void> activate() async {
    try {
      await _channel.invokeMethod<void>('activateDiagonalResizeCursor', {
        'device': device,
        'kind': cursor.kind,
      });
    } on MissingPluginException {
      await SystemChannels.mouseCursor.invokeMethod<void>(
        'activateSystemCursor',
        {'device': device, 'kind': 'grab'},
      );
    }
  }

  @override
  void dispose() {}
}

class _QuickMarkPanelLayoutDelegate extends SingleChildLayoutDelegate {
  final double anchorX;

  const _QuickMarkPanelLayoutDelegate({required this.anchorX});

  @override
  BoxConstraints getConstraintsForChild(BoxConstraints constraints) {
    return BoxConstraints(maxHeight: constraints.maxHeight);
  }

  @override
  Offset getPositionForChild(Size size, Size childSize) {
    final maxLeft = math.max(8.0, size.width - childSize.width - 8.0);
    final left = (anchorX - childSize.width / 2).clamp(8.0, maxLeft).toDouble();
    return Offset(left, 0);
  }

  @override
  bool shouldRelayout(covariant _QuickMarkPanelLayoutDelegate oldDelegate) {
    return oldDelegate.anchorX != anchorX;
  }
}

class _QuickMarkPanelSeparator extends StatelessWidget {
  const _QuickMarkPanelSeparator();

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Container(
      width: 1,
      height: 20,
      color: colorScheme.outlineVariant.withValues(alpha: 0.72),
    );
  }
}

ButtonStyle _quickMarkToggleButtonStyle(
  bool selected,
  ColorScheme colorScheme,
  Size size,
) {
  return IconButton.styleFrom(
    backgroundColor: selected
        ? colorScheme.primary.withValues(alpha: 0.16)
        : Colors.transparent,
    padding: EdgeInsets.zero,
    fixedSize: size,
    minimumSize: size,
    maximumSize: size,
    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
    hoverColor: selected
        ? colorScheme.primary.withValues(alpha: 0.20)
        : colorScheme.onSurfaceVariant.withValues(alpha: 0.10),
    focusColor: selected
        ? colorScheme.primary.withValues(alpha: 0.22)
        : colorScheme.onSurfaceVariant.withValues(alpha: 0.12),
    highlightColor: selected
        ? colorScheme.primary.withValues(alpha: 0.24)
        : colorScheme.onSurfaceVariant.withValues(alpha: 0.14),
  );
}

class _QuickMarkDeleteButton extends StatelessWidget {
  final VoidCallback? onPressed;

  const _QuickMarkDeleteButton({required this.onPressed});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    return Tooltip(
      message: l.delete,
      child: SizedBox(
        width: 28,
        height: 28,
        child: IconButton(
          visualDensity: VisualDensity.compact,
          padding: EdgeInsets.zero,
          iconSize: 18,
          constraints: const BoxConstraints.tightFor(width: 28, height: 28),
          style: _quickMarkDeleteButtonStyle(colorScheme),
          onPressed: onPressed,
          icon: const Icon(Icons.delete_outline),
        ),
      ),
    );
  }
}

class _QuickMarkColorOption extends StatelessWidget {
  final Color color;
  final String label;
  final bool selected;

  const _QuickMarkColorOption({
    required this.color,
    required this.label,
    required this.selected,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Row(
      children: [
        SizedBox(
          width: 22,
          child: selected
              ? Icon(Icons.check, size: 16, color: colorScheme.primary)
              : null,
        ),
        _QuickMarkColorSwatch(
          color: color,
          selected: selected,
          colorScheme: colorScheme,
        ),
        const SizedBox(width: 10),
        Text(
          label,
          style: Theme.of(context).textTheme.bodySmall?.copyWith(
            color: selected ? colorScheme.primary : colorScheme.onSurface,
          ),
        ),
      ],
    );
  }
}

class _QuickMarkColorSwatch extends StatelessWidget {
  final Color color;
  final bool selected;
  final ColorScheme colorScheme;

  const _QuickMarkColorSwatch({
    required this.color,
    required this.selected,
    required this.colorScheme,
  });

  @override
  Widget build(BuildContext context) {
    final isWhite = color.computeLuminance() > 0.9;
    return DecoratedBox(
      decoration: BoxDecoration(
        color: color,
        shape: BoxShape.circle,
        border: Border.all(
          color: selected
              ? colorScheme.primary
              : isWhite
              ? colorScheme.outline
              : colorScheme.outlineVariant,
          width: selected ? 2 : 1,
        ),
      ),
      child: const SizedBox(width: 14, height: 14),
    );
  }
}

class _QuickMarkStrokeOption extends StatelessWidget {
  final String label;
  final double width;
  final bool selected;

  const _QuickMarkStrokeOption({
    required this.label,
    required this.width,
    required this.selected,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Row(
      children: [
        SizedBox(
          width: 22,
          child: selected
              ? Icon(Icons.check, size: 16, color: colorScheme.primary)
              : null,
        ),
        SizedBox(
          width: 34,
          child: Text(
            label,
            style: Theme.of(context).textTheme.bodySmall?.copyWith(
              color: selected ? colorScheme.primary : colorScheme.onSurface,
              fontWeight: selected ? FontWeight.w600 : null,
            ),
          ),
        ),
        const SizedBox(width: 10),
        _QuickMarkStrokePreview(width: width),
      ],
    );
  }
}

class _QuickMarkStrokePreview extends StatelessWidget {
  final double width;
  final double previewWidth;

  const _QuickMarkStrokePreview({required this.width, this.previewWidth = 20});

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Container(
      width: previewWidth,
      height: width.clamp(1.0, 8.0).toDouble(),
      decoration: BoxDecoration(
        color: colorScheme.onSurfaceVariant,
        borderRadius: BorderRadius.circular(1),
      ),
    );
  }
}

ButtonStyle _quickMarkDeleteButtonStyle(ColorScheme colorScheme) {
  const size = Size.square(28);
  final warningStates = {
    WidgetState.hovered,
    WidgetState.focused,
    WidgetState.pressed,
  };
  return ButtonStyle(
    padding: const WidgetStatePropertyAll(EdgeInsets.zero),
    fixedSize: const WidgetStatePropertyAll(size),
    minimumSize: const WidgetStatePropertyAll(size),
    maximumSize: const WidgetStatePropertyAll(size),
    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
    shape: WidgetStatePropertyAll(
      RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
    ),
    foregroundColor: WidgetStateProperty.resolveWith((states) {
      if (states.any(warningStates.contains)) return colorScheme.error;
      return colorScheme.onSurfaceVariant;
    }),
    backgroundColor: WidgetStateProperty.resolveWith((states) {
      if (states.any(warningStates.contains)) {
        return colorScheme.error.withValues(alpha: 0.12);
      }
      return Colors.transparent;
    }),
    overlayColor: const WidgetStatePropertyAll(Colors.transparent),
  );
}

class _QuickMarkHandle extends StatelessWidget {
  const _QuickMarkHandle();
  static const double _size = 12.0;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: _size,
      height: _size,
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: Colors.white,
          border: Border.all(color: Colors.black.withValues(alpha: 0.72)),
          boxShadow: [
            BoxShadow(
              color: Colors.black.withValues(alpha: 0.35),
              blurRadius: 4,
            ),
          ],
        ),
      ),
    );
  }
}
