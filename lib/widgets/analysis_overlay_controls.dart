import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../analysis/analysis_overlay.dart';
import '../analysis/analysis_toolbar_data_source.dart';
import '../l10n/app_localizations.dart';
import '../track_manager.dart';
import '../utils/async_guard.dart';

const analysisOverlayControlBarKey = Key('analysis-overlay-control-bar');
const analysisOverlayOpacityKey = ValueKey('analysis-overlay-opacity');
const analysisOverlayStripKey = ValueKey('analysis-overlay-strip');

class AnalysisOverlayStrip extends StatefulWidget {
  static const double height = 34.0;

  final List<TrackEntry> entries;
  final AnalysisToolbarDataSource dataSource;
  final bool visible;
  final ValueChanged<AnalysisOverlayType> onTypeChanged;
  final ValueChanged<double> onOpacityChanged;
  final Future<void> Function() onActivateOverlay;
  final VoidCallback onDeactivateOverlay;
  final VoidCallback onClose;

  const AnalysisOverlayStrip({
    super.key,
    required this.entries,
    required this.dataSource,
    required this.visible,
    required this.onTypeChanged,
    required this.onOpacityChanged,
    required this.onActivateOverlay,
    required this.onDeactivateOverlay,
    required this.onClose,
  });

  @override
  State<AnalysisOverlayStrip> createState() => _AnalysisOverlayStripState();
}

class _AnalysisOverlayStripState extends State<AnalysisOverlayStrip> {
  Set<int> _cachedTrackFileIds = const {};
  Future<void>? _cacheRefreshInFlight;
  String? _lastDataSourceSignature;

  @override
  void initState() {
    super.initState();
    widget.dataSource.addListener(_handleDataSourceChanged);
    _lastDataSourceSignature = _dataSourceSignature();
    if (widget.visible) {
      fireAndLog(
        'refresh analysis overlay cached tracks',
        _refreshCachedTrackFileIds(),
      );
    }
  }

  @override
  void didUpdateWidget(covariant AnalysisOverlayStrip oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.dataSource != widget.dataSource) {
      oldWidget.dataSource.removeListener(_handleDataSourceChanged);
      widget.dataSource.addListener(_handleDataSourceChanged);
      _lastDataSourceSignature = _dataSourceSignature();
      _cachedTrackFileIds = const {};
    }
    if (!widget.visible || widget.entries.isEmpty) {
      if (_cachedTrackFileIds.isNotEmpty) {
        setState(() => _cachedTrackFileIds = const {});
      }
      return;
    }
    if (!listEquals(oldWidget.entries, widget.entries) ||
        !oldWidget.visible && widget.visible) {
      fireAndLog(
        'refresh analysis overlay cached tracks',
        _refreshCachedTrackFileIds(),
      );
    }
  }

  @override
  void dispose() {
    widget.dataSource.removeListener(_handleDataSourceChanged);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (!widget.visible || widget.entries.isEmpty) {
      return const SizedBox.shrink();
    }
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    return Semantics(
      container: true,
      explicitChildNodes: true,
      label: l.analysisOverlayControls,
      child: DecoratedBox(
        key: analysisOverlayStripKey,
        decoration: BoxDecoration(
          color: colorScheme.surfaceContainerLow.withValues(alpha: 0.92),
          border: Border(
            top: BorderSide(
              color: colorScheme.outlineVariant.withValues(alpha: 0.6),
            ),
            bottom: BorderSide(
              color: colorScheme.outlineVariant.withValues(alpha: 0.44),
            ),
          ),
        ),
        child: SizedBox(
          height: AnalysisOverlayStrip.height,
          child: Padding(
            padding: const EdgeInsets.fromLTRB(4, 2, 4, 2),
            child: Row(
              children: [
                Expanded(
                  child: AnalysisOverlayControlBar(
                    dataSource: widget.dataSource,
                    panelReady: _cachedTrackFileIds.isNotEmpty,
                    panelActive: widget.dataSource.overlayPanelVisible,
                    onTypeChanged: widget.onTypeChanged,
                    onOpacityChanged: widget.onOpacityChanged,
                    onActivateOverlay: widget.onActivateOverlay,
                    onDeactivateOverlay: widget.onDeactivateOverlay,
                  ),
                ),
                const SizedBox(width: 4),
                SizedBox(
                  width: 28,
                  height: 28,
                  child: IconButton(
                    onPressed: widget.onClose,
                    icon: const Icon(Icons.close, size: 15),
                    tooltip: MaterialLocalizations.of(
                      context,
                    ).closeButtonTooltip,
                    padding: EdgeInsets.zero,
                    constraints: const BoxConstraints.tightFor(
                      width: 28,
                      height: 28,
                    ),
                    style: ButtonStyle(
                      tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                      shape: WidgetStatePropertyAll(
                        RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(6),
                        ),
                      ),
                      foregroundColor: WidgetStatePropertyAll(
                        colorScheme.onSurfaceVariant,
                      ),
                      overlayColor: WidgetStatePropertyAll(
                        colorScheme.primary.withValues(alpha: 0.10),
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Future<void> _refreshCachedTrackFileIds() async {
    final existing = _cacheRefreshInFlight;
    if (existing != null) return existing;
    final entries = List<TrackEntry>.of(widget.entries);
    if (entries.isEmpty) return;
    late final Future<void> future;
    future =
        (() async {
          final snapshot = await widget.dataSource.snapshot();
          if (!mounted) return;
          final ready = <int>{};
          for (final entry in entries) {
            final status = widget.dataSource.statusForPath(entry.path);
            final statusHash = status?.hash;
            if ((status?.isCached ?? false) &&
                statusHash != null &&
                widget.dataSource.supportsOverlayForHash(statusHash)) {
              ready.add(entry.fileId);
              continue;
            }
            for (final cacheEntry in snapshot.entries) {
              if (cacheEntry.videoPath == entry.path &&
                  cacheEntry.complete &&
                  widget.dataSource.supportsOverlayForHash(cacheEntry.hash)) {
                ready.add(entry.fileId);
                break;
              }
            }
          }
          if (setEquals(ready, _cachedTrackFileIds)) return;
          setState(() => _cachedTrackFileIds = ready);
        })().whenComplete(() {
          if (identical(_cacheRefreshInFlight, future)) {
            _cacheRefreshInFlight = null;
          }
        });
    _cacheRefreshInFlight = future;
    return future;
  }

  void _handleDataSourceChanged() {
    if (!mounted) return;
    final signature = _dataSourceSignature();
    if (signature == _lastDataSourceSignature) return;
    _lastDataSourceSignature = signature;
    if (widget.visible) setState(() {});
  }

  String _dataSourceSignature() {
    final activeTrackIds = widget.dataSource.activeOverlayTrackFileIds.toList()
      ..sort();
    final config = widget.dataSource.overlayConfig;
    return [
      widget.dataSource.state.name,
      widget.dataSource.overlayPanelVisible,
      activeTrackIds.join('|'),
      config.type.name,
      config.opacity.toStringAsFixed(3),
    ].join(';');
  }
}

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
    final waitingForCache = !panelReady && !panelActive;
    return DecoratedBox(
      key: analysisOverlayControlBarKey,
      decoration: BoxDecoration(
        color: colorScheme.surface.withValues(alpha: 0.86),
        borderRadius: BorderRadius.circular(6),
        border: Border.all(
          color: panelActive
              ? colorScheme.primary.withValues(alpha: 0.58)
              : colorScheme.outlineVariant.withValues(
                  alpha: waitingForCache ? 0.38 : 0.28,
                ),
        ),
      ),
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
              if (!panelActive && activate != null) {
                fireAndLog('activate analysis overlay', activate());
              }
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

    return Semantics(
      button: true,
      selected: widget.selected,
      label: widget.tooltip,
      onTap: widget.onPressed,
      child: ExcludeSemantics(
        child: Tooltip(
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
    const step = 0.05;
    return Semantics(
      slider: true,
      label: tooltip,
      value: '${(normalized * 100).round()}%',
      increasedValue: '${((normalized + step).clamp(0.0, 1.0) * 100).round()}%',
      decreasedValue: '${((normalized - step).clamp(0.0, 1.0) * 100).round()}%',
      onIncrease: normalized < 1.0
          ? () => onChanged((normalized + step).clamp(0.0, 1.0))
          : null,
      onDecrease: normalized > 0.0
          ? () => onChanged((normalized - step).clamp(0.0, 1.0))
          : null,
      child: ExcludeSemantics(
        child: Tooltip(
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
                  onTapDown: (details) =>
                      updateFromLocal(details.localPosition),
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
        ),
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
