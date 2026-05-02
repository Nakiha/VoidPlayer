import 'package:flutter/material.dart';

import '../../../l10n/app_localizations.dart';
import 'analysis_style.dart';

// ===========================================================================
// Top bar widgets
// ===========================================================================

class AnalysisOrderToggle extends StatelessWidget {
  final bool ptsOrder;
  final ValueChanged<bool> onChanged;
  final AppLocalizations l;
  const AnalysisOrderToggle({
    super.key,
    required this.ptsOrder,
    required this.onChanged,
    required this.l,
  });

  @override
  Widget build(BuildContext context) {
    return ExcludeSemantics(
      child: SegmentedButton<bool>(
        showSelectedIcon: false,
        segments: [
          ButtonSegment(value: true, label: Text(l.analysisPtsOrder)),
          ButtonSegment(value: false, label: Text(l.analysisDtsOrder)),
        ],
        selected: {ptsOrder},
        onSelectionChanged: (s) => onChanged(s.first),
        style: const ButtonStyle(
          visualDensity: VisualDensity.compact,
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          textStyle: WidgetStatePropertyAll(
            TextStyle(fontSize: 12, fontWeight: FontWeight.w700),
          ),
          fixedSize: WidgetStatePropertyAll(
            Size.fromHeight(analysisHeaderControlHeight),
          ),
        ),
      ),
    );
  }
}

class AnalysisViewTabBar extends StatelessWidget {
  final int selectedTab;
  final ValueChanged<int> onTabChanged;
  final AppLocalizations l;
  const AnalysisViewTabBar({
    super.key,
    required this.selectedTab,
    required this.onTabChanged,
    required this.l,
  });

  @override
  Widget build(BuildContext context) {
    return ExcludeSemantics(
      child: SegmentedButton<int>(
        showSelectedIcon: false,
        segments: [
          ButtonSegment(
            value: 0,
            label: Tooltip(
              message: l.analysisRefPyramid,
              child: const SizedBox(
                width: 28,
                height: 20,
                child: _AnalysisViewIcon(_AnalysisViewIconKind.pyramid),
              ),
            ),
          ),
          ButtonSegment(
            value: 1,
            label: Tooltip(
              message: l.analysisFrameTrend,
              child: const SizedBox(
                width: 28,
                height: 20,
                child: _AnalysisViewIcon(_AnalysisViewIconKind.trend),
              ),
            ),
          ),
        ],
        selected: {selectedTab},
        onSelectionChanged: (s) => onTabChanged(s.first),
        style: const ButtonStyle(
          visualDensity: VisualDensity.compact,
          padding: WidgetStatePropertyAll(EdgeInsets.symmetric(horizontal: 7)),
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          textStyle: WidgetStatePropertyAll(TextStyle(fontSize: 12)),
          fixedSize: WidgetStatePropertyAll(
            Size.fromHeight(analysisHeaderControlHeight),
          ),
        ),
      ),
    );
  }
}

enum _AnalysisViewIconKind { pyramid, trend }

class _AnalysisViewIcon extends StatelessWidget {
  final _AnalysisViewIconKind kind;

  const _AnalysisViewIcon(this.kind);

  @override
  Widget build(BuildContext context) {
    final color =
        IconTheme.of(context).color ??
        DefaultTextStyle.of(context).style.color ??
        Theme.of(context).colorScheme.onSurface;
    return CustomPaint(painter: _AnalysisViewIconPainter(kind, color));
  }
}

class _AnalysisViewIconPainter extends CustomPainter {
  final _AnalysisViewIconKind kind;
  final Color color;

  const _AnalysisViewIconPainter(this.kind, this.color);

  @override
  void paint(Canvas canvas, Size size) {
    final stroke = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.35
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;
    final fill = Paint()
      ..color = color.withValues(alpha: 0.92)
      ..style = PaintingStyle.fill;

    switch (kind) {
      case _AnalysisViewIconKind.pyramid:
        final p0 = Offset(size.width * 0.18, size.height * 0.76);
        final p1 = Offset(size.width * 0.42, size.height * 0.46);
        final p2 = Offset(size.width * 0.68, size.height * 0.22);
        final p3 = Offset(size.width * 0.84, size.height * 0.58);
        canvas.drawLine(p0, p1, stroke);
        canvas.drawLine(p1, p2, stroke);
        canvas.drawLine(p1, p3, stroke);
        canvas.drawLine(p2, p3, stroke..color = color.withValues(alpha: 0.55));
        for (final p in [p0, p1, p2, p3]) {
          canvas.drawCircle(p, 2.25, fill);
        }

      case _AnalysisViewIconKind.trend:
        final baseY = size.height * 0.76;
        final barW = size.width * 0.085;
        final xs = [
          size.width * 0.20,
          size.width * 0.40,
          size.width * 0.60,
          size.width * 0.80,
        ];
        final hs = [
          size.height * 0.30,
          size.height * 0.50,
          size.height * 0.24,
          size.height * 0.60,
        ];
        for (var i = 0; i < xs.length; i++) {
          canvas.drawRRect(
            RRect.fromRectAndRadius(
              Rect.fromLTWH(xs[i], baseY - hs[i], barW, hs[i]),
              const Radius.circular(1.5),
            ),
            fill,
          );
        }
        final line = Path()
          ..moveTo(size.width * 0.12, size.height * 0.64)
          ..lineTo(size.width * 0.35, size.height * 0.54)
          ..lineTo(size.width * 0.56, size.height * 0.62)
          ..lineTo(size.width * 0.86, size.height * 0.36);
        canvas.drawPath(line, stroke);
    }
  }

  @override
  bool shouldRepaint(covariant _AnalysisViewIconPainter oldDelegate) =>
      kind != oldDelegate.kind || color != oldDelegate.color;
}

// ===========================================================================
// Resizable vertical divider (drag to change left panel width)
// ===========================================================================

class AnalysisResizableVDivider extends StatefulWidget {
  final double position;
  final ValueChanged<double> onPositionChanged;

  const AnalysisResizableVDivider({
    super.key,
    required this.position,
    required this.onPositionChanged,
  });

  @override
  State<AnalysisResizableVDivider> createState() =>
      _AnalysisResizableVDividerState();
}

class _AnalysisResizableVDividerState extends State<AnalysisResizableVDivider> {
  bool _hovering = false;
  double _excess = 0.0;
  late double _effectivePos;

  @override
  void initState() {
    super.initState();
    _effectivePos = widget.position;
  }

  @override
  void didUpdateWidget(covariant AnalysisResizableVDivider oldWidget) {
    super.didUpdateWidget(oldWidget);
    _effectivePos = widget.position;
  }

  void _onDragStart(_) {
    _excess = 0.0;
    _effectivePos = widget.position;
  }

  void _onDragUpdate(DragUpdateDetails details) {
    final desired = _effectivePos + _excess + details.delta.dx;
    final clamped = desired.clamp(120.0, double.maxFinite);
    _excess = desired - clamped;
    _effectivePos = clamped;
    widget.onPositionChanged(clamped);
  }

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onHorizontalDragStart: _onDragStart,
      onHorizontalDragUpdate: _onDragUpdate,
      child: MouseRegion(
        cursor: SystemMouseCursors.resizeColumn,
        onEnter: (_) => setState(() => _hovering = true),
        onExit: (_) => setState(() => _hovering = false),
        child: SizedBox.expand(
          child: Center(
            child: Container(
              width: _hovering ? 2 : 0,
              color: _hovering
                  ? Theme.of(context).colorScheme.primary
                  : Colors.transparent,
            ),
          ),
        ),
      ),
    );
  }
}

class AnalysisResizableHDivider extends StatefulWidget {
  final double position;
  final double minPosition;
  final double maxPosition;
  final ValueChanged<double> onPositionChanged;

  const AnalysisResizableHDivider({
    super.key,
    required this.position,
    required this.minPosition,
    required this.maxPosition,
    required this.onPositionChanged,
  });

  @override
  State<AnalysisResizableHDivider> createState() =>
      _AnalysisResizableHDividerState();
}

class _AnalysisResizableHDividerState extends State<AnalysisResizableHDivider> {
  bool _hovering = false;
  double _dragStartGlobalY = 0.0;
  double _dragStartPosition = 0.0;

  void _onDragStart(DragStartDetails details) {
    _dragStartGlobalY = details.globalPosition.dy;
    _dragStartPosition = widget.position;
  }

  void _onDragUpdate(DragUpdateDetails details) {
    final desired =
        _dragStartPosition + details.globalPosition.dy - _dragStartGlobalY;
    final clamped = desired.clamp(widget.minPosition, widget.maxPosition);
    widget.onPositionChanged(clamped);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onVerticalDragStart: _onDragStart,
      onVerticalDragUpdate: _onDragUpdate,
      child: MouseRegion(
        cursor: SystemMouseCursors.resizeRow,
        onEnter: (_) => setState(() => _hovering = true),
        onExit: (_) => setState(() => _hovering = false),
        child: SizedBox.expand(
          child: Align(
            alignment: Alignment.topCenter,
            child: SizedBox(
              width: double.infinity,
              height: _hovering ? 2 : 1,
              child: ColoredBox(
                color: _hovering
                    ? theme.colorScheme.primary
                    : theme.colorScheme.outlineVariant,
              ),
            ),
          ),
        ),
      ),
    );
  }
}
