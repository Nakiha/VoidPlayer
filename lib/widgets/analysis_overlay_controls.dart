import 'package:flutter/material.dart';

import '../analysis/analysis_overlay.dart';
import '../analysis/analysis_toolbar_data_source.dart';
import '../l10n/app_localizations.dart';
import '../track_manager.dart';

const analysisOverlayControlBarKey = Key('analysis-overlay-control-bar');

class AnalysisOverlayControlBar extends StatefulWidget {
  static const double margin = 4.0;

  final List<TrackEntry> entries;
  final AnalysisToolbarDataSource dataSource;
  final ValueChanged<AnalysisOverlayType> onTypeChanged;
  final ValueChanged<Set<AnalysisOverlayLayer>> onLayersChanged;
  final ValueChanged<double> onOpacityChanged;

  const AnalysisOverlayControlBar({
    super.key,
    required this.entries,
    required this.dataSource,
    required this.onTypeChanged,
    required this.onLayersChanged,
    required this.onOpacityChanged,
  });

  @override
  State<AnalysisOverlayControlBar> createState() =>
      _AnalysisOverlayControlBarState();
}

class _AnalysisOverlayControlBarState extends State<AnalysisOverlayControlBar> {
  bool _syncSettingsToAll = true;
  int? _primaryTrackFileId;

  @override
  void initState() {
    super.initState();
    _syncPrimaryTrack();
  }

  @override
  void didUpdateWidget(covariant AnalysisOverlayControlBar oldWidget) {
    super.didUpdateWidget(oldWidget);
    _syncPrimaryTrack();
  }

  @override
  Widget build(BuildContext context) {
    if (widget.entries.isEmpty || !widget.dataSource.overlayPanelVisible) {
      return const SizedBox.shrink();
    }
    final overlayTrackIds = widget.dataSource.activeOverlayTrackFileIds;
    final primaryTrackFileId = _primaryTrackFileId;
    return Row(
      key: analysisOverlayControlBarKey,
      children: [
        for (int i = 0; i < widget.entries.length; i++) ...[
          if (i > 0) const SizedBox(width: 4),
          Expanded(
            child: _AnalysisOverlayTrackPanel(
              track: widget.entries[i],
              primary: widget.entries[i].fileId == primaryTrackFileId,
              overlayReady: overlayTrackIds.contains(widget.entries[i].fileId),
              syncSettingsToAll: _syncSettingsToAll,
              config: widget.dataSource.overlayConfig,
              onTypeChanged: widget.onTypeChanged,
              onLayersChanged: widget.onLayersChanged,
              onOpacityChanged: widget.onOpacityChanged,
              onSyncSettingsToAllChanged: (value) {
                setState(() {
                  _syncSettingsToAll = value;
                });
              },
            ),
          ),
        ],
      ],
    );
  }

  void _syncPrimaryTrack() {
    if (widget.entries.isEmpty) {
      _primaryTrackFileId = null;
      return;
    }
    final overlayTrackIds = widget.dataSource.activeOverlayTrackFileIds;
    final current = _primaryTrackFileId;
    if (current != null &&
        widget.entries.any((entry) => entry.fileId == current) &&
        overlayTrackIds.contains(current)) {
      return;
    }
    for (final entry in widget.entries) {
      if (overlayTrackIds.contains(entry.fileId)) {
        _primaryTrackFileId = entry.fileId;
        return;
      }
    }
    _primaryTrackFileId = widget.entries.first.fileId;
  }
}

class _AnalysisOverlayTrackPanel extends StatelessWidget {
  final TrackEntry track;
  final bool primary;
  final bool overlayReady;
  final bool syncSettingsToAll;
  final AnalysisOverlayConfig config;
  final ValueChanged<AnalysisOverlayType> onTypeChanged;
  final ValueChanged<Set<AnalysisOverlayLayer>> onLayersChanged;
  final ValueChanged<double> onOpacityChanged;
  final ValueChanged<bool> onSyncSettingsToAllChanged;

  const _AnalysisOverlayTrackPanel({
    required this.track,
    required this.primary,
    required this.overlayReady,
    required this.syncSettingsToAll,
    required this.config,
    required this.onTypeChanged,
    required this.onLayersChanged,
    required this.onOpacityChanged,
    required this.onSyncSettingsToAllChanged,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final editable = overlayReady && (primary || !syncSettingsToAll);
    final contentOpacity = editable ? 1.0 : 0.42;

    return ExcludeSemantics(
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: colorScheme.surface.withValues(alpha: 0.86),
          borderRadius: BorderRadius.circular(6),
          border: Border.all(
            color: primary
                ? colorScheme.primary.withValues(alpha: 0.58)
                : colorScheme.outlineVariant.withValues(alpha: 0.28),
          ),
        ),
        child: Opacity(
          opacity: contentOpacity,
          child: IgnorePointer(
            ignoring: !editable,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Row(
                  children: [
                    Expanded(
                      child: _OverlayTypeButtons(
                        trackFileId: track.fileId,
                        value: config.type,
                        onChanged: onTypeChanged,
                      ),
                    ),
                    const SizedBox(width: 4),
                    if (primary) ...[
                      _OverlayPanelDivider(),
                      const SizedBox(width: 4),
                      _OverlayIconButton(
                        key: ValueKey('analysis-overlay-sync-${track.fileId}'),
                        selected: syncSettingsToAll,
                        icon: Icons.sync,
                        tooltip: AppLocalizations.of(
                          context,
                        )!.analysisOverlaySyncSettings,
                        onPressed: () =>
                            onSyncSettingsToAllChanged(!syncSettingsToAll),
                      ),
                      const SizedBox(width: 4),
                    ],
                  ],
                ),
                const SizedBox(height: 4),
                Row(
                  children: [
                    _OverlayLayerButton(
                      trackFileId: track.fileId,
                      layer: AnalysisOverlayLayer.cuGrid,
                      selected: config.layers.contains(
                        AnalysisOverlayLayer.cuGrid,
                      ),
                      onChanged: _toggleLayer,
                    ),
                    _OverlayLayerButton(
                      trackFileId: track.fileId,
                      layer: AnalysisOverlayLayer.predictionMode,
                      selected: config.layers.contains(
                        AnalysisOverlayLayer.predictionMode,
                      ),
                      onChanged: _toggleLayer,
                    ),
                    _OverlayLayerButton(
                      trackFileId: track.fileId,
                      layer: AnalysisOverlayLayer.predictionLines,
                      selected: config.layers.contains(
                        AnalysisOverlayLayer.predictionLines,
                      ),
                      onChanged: _toggleLayer,
                    ),
                    const SizedBox(width: 6),
                    Expanded(
                      child: _OverlayOpacityScrubber(
                        key: ValueKey(
                          'analysis-overlay-opacity-${track.fileId}',
                        ),
                        value: config.opacity,
                        onChanged: onOpacityChanged,
                      ),
                    ),
                    ExcludeSemantics(
                      child: SizedBox(
                        width: 34,
                        child: Text(
                          '${(config.opacity * 100).round()}%',
                          textAlign: TextAlign.right,
                          style: theme.textTheme.labelSmall?.copyWith(
                            color: colorScheme.onSurfaceVariant,
                            fontFeatures: const [FontFeature.tabularFigures()],
                          ),
                        ),
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  void _toggleLayer(AnalysisOverlayLayer layer, bool selected) {
    final layers = Set<AnalysisOverlayLayer>.of(config.layers);
    selected ? layers.add(layer) : layers.remove(layer);
    onLayersChanged(layers);
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
  final int trackFileId;
  final AnalysisOverlayType value;
  final ValueChanged<AnalysisOverlayType> onChanged;

  const _OverlayTypeButtons({
    required this.trackFileId,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        for (final type in AnalysisOverlayType.values)
          _OverlayIconButton(
            key: ValueKey('analysis-overlay-type-$trackFileId-${type.name}'),
            selected: value == type,
            icon: _iconForType(type),
            tooltip: _tooltipForType(context, type),
            onPressed: () => onChanged(type),
          ),
      ],
    );
  }

  IconData _iconForType(AnalysisOverlayType type) => switch (type) {
    AnalysisOverlayType.cu => Icons.grid_4x4,
    AnalysisOverlayType.prediction => Icons.category_outlined,
    AnalysisOverlayType.predictionLines => Icons.polyline,
    AnalysisOverlayType.qpHeatmap => Icons.thermostat,
    AnalysisOverlayType.cuBitCostHeatmap => Icons.show_chart,
  };

  String _tooltipForType(BuildContext context, AnalysisOverlayType type) {
    final l = AppLocalizations.of(context)!;
    return switch (type) {
      AnalysisOverlayType.cu => l.analysisOverlayTypeCu,
      AnalysisOverlayType.prediction => l.analysisOverlayTypePrediction,
      AnalysisOverlayType.predictionLines =>
        l.analysisOverlayTypePredictionLines,
      AnalysisOverlayType.qpHeatmap => l.analysisOverlayTypeQpHeatmap,
      AnalysisOverlayType.cuBitCostHeatmap =>
        l.analysisOverlayTypeCuBitCostHeatmap,
    };
  }
}

class _OverlayLayerButton extends StatelessWidget {
  final int trackFileId;
  final AnalysisOverlayLayer layer;
  final bool selected;
  final void Function(AnalysisOverlayLayer layer, bool selected) onChanged;

  const _OverlayLayerButton({
    required this.trackFileId,
    required this.layer,
    required this.selected,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    return _OverlayIconButton(
      key: ValueKey('analysis-overlay-layer-$trackFileId-${layer.name}'),
      selected: selected,
      icon: _iconForLayer(layer),
      tooltip: _tooltipForLayer(context, layer),
      onPressed: () => onChanged(layer, !selected),
    );
  }

  IconData _iconForLayer(AnalysisOverlayLayer layer) => switch (layer) {
    AnalysisOverlayLayer.cuGrid => Icons.grid_on,
    AnalysisOverlayLayer.predictionMode => Icons.label_outline,
    AnalysisOverlayLayer.predictionLines => Icons.alt_route,
  };

  String _tooltipForLayer(BuildContext context, AnalysisOverlayLayer layer) {
    final l = AppLocalizations.of(context)!;
    return switch (layer) {
      AnalysisOverlayLayer.cuGrid => l.analysisOverlayLayerCuGrid,
      AnalysisOverlayLayer.predictionMode =>
        l.analysisOverlayLayerPredictionMode,
      AnalysisOverlayLayer.predictionLines =>
        l.analysisOverlayLayerPredictionLines,
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

    return ExcludeSemantics(
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        onEnter: (_) => setState(() => _hovered = true),
        onExit: (_) => setState(() {
          _hovered = false;
          _pressed = false;
        }),
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          excludeFromSemantics: true,
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

class _OverlayOpacityScrubber extends StatefulWidget {
  final double value;
  final ValueChanged<double> onChanged;

  const _OverlayOpacityScrubber({
    super.key,
    required this.value,
    required this.onChanged,
  });

  @override
  State<_OverlayOpacityScrubber> createState() =>
      _OverlayOpacityScrubberState();
}

class _OverlayOpacityScrubberState extends State<_OverlayOpacityScrubber> {
  bool _hovered = false;

  @override
  Widget build(BuildContext context) {
    return ExcludeSemantics(
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        onEnter: (_) => setState(() => _hovered = true),
        onExit: (_) => setState(() => _hovered = false),
        child: LayoutBuilder(
          builder: (context, constraints) {
            return GestureDetector(
              behavior: HitTestBehavior.opaque,
              excludeFromSemantics: true,
              onTapDown: (details) => _updateFromLocalDx(
                details.localPosition.dx,
                constraints.maxWidth,
              ),
              onHorizontalDragUpdate: (details) => _updateFromLocalDx(
                details.localPosition.dx,
                constraints.maxWidth,
              ),
              child: SizedBox(
                height: 24,
                child: CustomPaint(
                  painter: _OverlayOpacityScrubberPainter(
                    value: widget.value,
                    hovered: _hovered,
                    colorScheme: Theme.of(context).colorScheme,
                  ),
                ),
              ),
            );
          },
        ),
      ),
    );
  }

  void _updateFromLocalDx(double dx, double width) {
    if (width <= 0) return;
    final normalized = (dx / width).clamp(0.0, 1.0).toDouble();
    widget.onChanged(0.1 + normalized * 0.9);
  }
}

class _OverlayOpacityScrubberPainter extends CustomPainter {
  final double value;
  final bool hovered;
  final ColorScheme colorScheme;

  const _OverlayOpacityScrubberPainter({
    required this.value,
    required this.hovered,
    required this.colorScheme,
  });

  @override
  void paint(Canvas canvas, Size size) {
    final centerY = size.height / 2;
    final trackLeft = 4.0;
    final trackRight = size.width - 4.0;
    final trackWidth = (trackRight - trackLeft).clamp(1.0, double.infinity);
    final normalized = ((value.clamp(0.1, 1.0) - 0.1) / 0.9).toDouble();
    final thumbX = trackLeft + trackWidth * normalized;

    final inactivePaint = Paint()
      ..color = colorScheme.outlineVariant.withValues(
        alpha: hovered ? 0.75 : 0.55,
      )
      ..strokeWidth = 2
      ..strokeCap = StrokeCap.round;
    final activePaint = Paint()
      ..color = colorScheme.primary.withValues(alpha: hovered ? 0.95 : 0.82)
      ..strokeWidth = 2.4
      ..strokeCap = StrokeCap.round;
    canvas.drawLine(
      Offset(trackLeft, centerY),
      Offset(trackRight, centerY),
      inactivePaint,
    );
    canvas.drawLine(
      Offset(trackLeft, centerY),
      Offset(thumbX, centerY),
      activePaint,
    );

    final thumbPaint = Paint()..color = colorScheme.primary;
    final ringPaint = Paint()
      ..color = colorScheme.surface
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.4;
    canvas.drawCircle(Offset(thumbX, centerY), hovered ? 6 : 5, thumbPaint);
    canvas.drawCircle(Offset(thumbX, centerY), hovered ? 6 : 5, ringPaint);
  }

  @override
  bool shouldRepaint(covariant _OverlayOpacityScrubberPainter oldDelegate) {
    return oldDelegate.value != value ||
        oldDelegate.hovered != hovered ||
        oldDelegate.colorScheme != colorScheme;
  }
}
