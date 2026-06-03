import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../analysis/analysis_overlay.dart';
import '../analysis/analysis_toolbar_data_source.dart';
import '../l10n/app_localizations.dart';

const analysisOverlayControlBarKey = Key('analysis-overlay-control-bar');
const analysisOverlayOpacityKey = ValueKey('analysis-overlay-opacity');

class AnalysisOverlayControlBar extends StatelessWidget {
  static const double margin = 4.0;
  static const double _innerPadding = 3.0;

  final AnalysisToolbarDataSource dataSource;
  final bool panelReady;
  final bool panelActive;
  final ValueChanged<AnalysisOverlayType> onTypeChanged;
  final ValueChanged<double> onOpacityChanged;
  final Future<void> Function()? onActivateOverlay;
  final VoidCallback? onDeactivateOverlay;

  const AnalysisOverlayControlBar({
    super.key,
    required this.dataSource,
    this.panelReady = true,
    this.panelActive = true,
    required this.onTypeChanged,
    required this.onOpacityChanged,
    this.onActivateOverlay,
    this.onDeactivateOverlay,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final config = dataSource.overlayConfig;
    final editable = panelReady || panelActive;
    return DecoratedBox(
      key: analysisOverlayControlBarKey,
      decoration: BoxDecoration(
        color: colorScheme.surface.withValues(alpha: 0.86),
        borderRadius: BorderRadius.circular(6),
        border: Border.all(
          color: panelActive
              ? colorScheme.primary.withValues(alpha: 0.58)
              : colorScheme.outlineVariant.withValues(alpha: 0.28),
        ),
      ),
      child: Opacity(
        opacity: editable ? 1.0 : 0.42,
        child: IgnorePointer(
          ignoring: !editable,
          child: SizedBox(
            height: 30,
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.center,
              children: [
                const SizedBox(width: AnalysisOverlayControlBar._innerPadding),
                _OverlayTypeButtons(
                  value: config.type,
                  panelActive: panelActive,
                  onChanged: onTypeChanged,
                  onActivateOverlay: onActivateOverlay,
                  onDeactivateOverlay: onDeactivateOverlay,
                ),
                const SizedBox(width: 6),
                _OverlayPanelDivider(),
                const SizedBox(width: 6),
                Expanded(
                  child: _OverlayOpacitySlider(
                    key: analysisOverlayOpacityKey,
                    value: config.opacity,
                    onChanged: onOpacityChanged,
                  ),
                ),
                const SizedBox(width: AnalysisOverlayControlBar._innerPadding),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _OverlayPanelDivider extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 1,
      height: 18,
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: Theme.of(
            context,
          ).colorScheme.outlineVariant.withValues(alpha: 0.62),
        ),
      ),
    );
  }
}

class _OverlayTypeButtons extends StatelessWidget {
  final AnalysisOverlayType value;
  final bool panelActive;
  final ValueChanged<AnalysisOverlayType> onChanged;
  final Future<void> Function()? onActivateOverlay;
  final VoidCallback? onDeactivateOverlay;

  const _OverlayTypeButtons({
    required this.value,
    required this.panelActive,
    required this.onChanged,
    required this.onActivateOverlay,
    required this.onDeactivateOverlay,
  });

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        for (final type in AnalysisOverlayType.values)
          _OverlayIconButton(
            key: ValueKey('analysis-overlay-type-${type.name}'),
            selected: panelActive && value == type,
            icon: _iconForType(type),
            tooltip: _tooltipForType(context, type),
            onPressed: () {
              if (panelActive && value == type) {
                onDeactivateOverlay?.call();
                return;
              }
              onChanged(type);
              final activate = onActivateOverlay;
              if (!panelActive && activate != null) unawaited(activate());
            },
          ),
      ],
    );
  }

  IconData _iconForType(AnalysisOverlayType type) => switch (type) {
    AnalysisOverlayType.cu => Icons.grid_4x4,
    AnalysisOverlayType.qpHeatmap => Icons.thermostat,
    AnalysisOverlayType.cuBitCostHeatmap => Icons.show_chart,
  };

  String _tooltipForType(BuildContext context, AnalysisOverlayType type) {
    final l = AppLocalizations.of(context)!;
    return switch (type) {
      AnalysisOverlayType.cu => l.analysisOverlayTypeCu,
      AnalysisOverlayType.qpHeatmap => l.analysisOverlayTypeQpHeatmap,
      AnalysisOverlayType.cuBitCostHeatmap =>
        l.analysisOverlayTypeCuBitCostHeatmap,
    };
  }
}

class _OverlayIconButton extends StatelessWidget {
  final bool selected;
  final IconData icon;
  final String tooltip;
  final VoidCallback onPressed;

  const _OverlayIconButton({
    super.key,
    required this.selected,
    required this.icon,
    required this.tooltip,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    return _OverlayBareIconButton(
      selected: selected,
      icon: icon,
      tooltip: tooltip,
      onPressed: onPressed,
    );
  }
}

class _OverlayBareIconButton extends StatefulWidget {
  final bool selected;
  final IconData icon;
  final String tooltip;
  final VoidCallback onPressed;

  const _OverlayBareIconButton({
    required this.selected,
    required this.icon,
    required this.tooltip,
    required this.onPressed,
  });

  @override
  State<_OverlayBareIconButton> createState() => _OverlayBareIconButtonState();
}

class _OverlayBareIconButtonState extends State<_OverlayBareIconButton> {
  bool _hovered = false;
  bool _pressed = false;

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    final background = widget.selected
        ? colorScheme.primary
        : _pressed
        ? colorScheme.primary.withValues(alpha: 0.18)
        : _hovered
        ? colorScheme.primary.withValues(alpha: 0.12)
        : Colors.transparent;
    final foreground = widget.selected
        ? colorScheme.onPrimary
        : colorScheme.onSurfaceVariant;

    return Tooltip(
      message: widget.tooltip,
      excludeFromSemantics: true,
      waitDuration: const Duration(milliseconds: 450),
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        onEnter: (_) => setState(() => _hovered = true),
        onExit: (_) => setState(() {
          _hovered = false;
          _pressed = false;
        }),
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onTap: widget.onPressed,
          onTapDown: (_) => setState(() => _pressed = true),
          onTapCancel: () => setState(() => _pressed = false),
          onTapUp: (_) => setState(() => _pressed = false),
          child: SizedBox(
            width: 26,
            height: 24,
            child: DecoratedBox(
              decoration: BoxDecoration(
                color: background,
                borderRadius: BorderRadius.circular(5),
              ),
              child: Center(
                child: Icon(widget.icon, size: 15, color: foreground),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _OverlayOpacitySlider extends StatelessWidget {
  final double value;
  final ValueChanged<double> onChanged;

  const _OverlayOpacitySlider({
    super.key,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final tooltip = AppLocalizations.of(context)!.analysisOverlayOpacity;
    final colorScheme = Theme.of(context).colorScheme;
    final normalized = value.clamp(0.0, 1.0).toDouble();
    return Tooltip(
      message: tooltip,
      excludeFromSemantics: true,
      waitDuration: const Duration(milliseconds: 450),
      child: LayoutBuilder(
        builder: (context, constraints) {
          void updateFromLocal(Offset local) {
            final width = constraints.maxWidth <= 0
                ? 1.0
                : constraints.maxWidth;
            final t = (local.dx / width).clamp(0.0, 1.0);
            onChanged(t.toDouble());
          }

          return MouseRegion(
            cursor: SystemMouseCursors.click,
            child: GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTapDown: (details) => updateFromLocal(details.localPosition),
              onHorizontalDragUpdate: (details) =>
                  updateFromLocal(details.localPosition),
              child: SizedBox(
                height: 24,
                child: CustomPaint(
                  painter: _OverlayOpacitySliderPainter(
                    normalized: normalized,
                    colorScheme: colorScheme,
                  ),
                ),
              ),
            ),
          );
        },
      ),
    );
  }
}

class _OverlayOpacitySliderPainter extends CustomPainter {
  final double normalized;
  final ColorScheme colorScheme;

  const _OverlayOpacitySliderPainter({
    required this.normalized,
    required this.colorScheme,
  });

  @override
  void paint(Canvas canvas, Size size) {
    final value = normalized.clamp(0.0, 1.0).toDouble();
    final centerY = size.height / 2;
    final trackLeft = 5.5;
    final trackRight = math.max(trackLeft, size.width - 5.5);
    final trackWidth = trackRight - trackLeft;
    final thumbX = trackLeft + trackWidth * value;
    final trackRect = RRect.fromLTRBR(
      trackLeft,
      centerY - 1.4,
      trackRight,
      centerY + 1.4,
      const Radius.circular(2),
    );
    final activeRect = RRect.fromLTRBR(
      trackLeft,
      centerY - 1.4,
      thumbX,
      centerY + 1.4,
      const Radius.circular(2),
    );

    canvas.drawRRect(
      trackRect,
      Paint()
        ..color = colorScheme.outlineVariant.withValues(alpha: 0.64)
        ..isAntiAlias = true,
    );
    canvas.drawRRect(
      activeRect,
      Paint()
        ..color = colorScheme.primary.withValues(alpha: 0.92)
        ..isAntiAlias = true,
    );
    canvas.drawCircle(
      Offset(thumbX, centerY),
      5.5,
      Paint()
        ..color = colorScheme.primary
        ..isAntiAlias = true,
    );
  }

  @override
  bool shouldRepaint(covariant _OverlayOpacitySliderPainter oldDelegate) {
    return oldDelegate.normalized != normalized ||
        oldDelegate.colorScheme != colorScheme;
  }
}
