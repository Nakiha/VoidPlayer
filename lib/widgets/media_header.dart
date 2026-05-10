import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import '../analysis/analysis_manager.dart';
import '../analysis/analysis_overlay.dart';
import '../analysis/analysis_toolbar_data_source.dart';
import '../l10n/app_localizations.dart';
import '../track_manager.dart';
import 'analysis_overlay_controls.dart';
import 'app_menu_combo.dart';

/// Bar of per-track media headers, placed between viewport and controls bar.
///
/// Each header shows a combo box for switching media sources and a remove button,
/// matching the PySide6 `MediaHeader`.
class MediaHeaderBar extends StatelessWidget {
  final List<TrackEntry> entries;
  final AnalysisToolbarDataSource analysisDataSource;
  final Key? analysisOverlayButtonKey;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final Future<void> Function() onAnalysisOverlayPanelToggle;
  final ValueChanged<AnalysisOverlayType> onAnalysisOverlayTypeChanged;
  final ValueChanged<Set<AnalysisOverlayLayer>> onAnalysisOverlayLayersChanged;
  final ValueChanged<double> onAnalysisOverlayOpacityChanged;
  final void Function(int slotIndex) onRemoveClicked;

  const MediaHeaderBar({
    super.key,
    required this.entries,
    required this.analysisDataSource,
    this.analysisOverlayButtonKey,
    required this.onMediaSwapped,
    required this.onAnalysisOverlayPanelToggle,
    required this.onAnalysisOverlayTypeChanged,
    required this.onAnalysisOverlayLayersChanged,
    required this.onAnalysisOverlayOpacityChanged,
    required this.onRemoveClicked,
  });

  @override
  Widget build(BuildContext context) {
    return _MediaHeaderBarWithCache(
      entries: entries,
      analysisDataSource: analysisDataSource,
      analysisOverlayButtonKey: analysisOverlayButtonKey,
      onMediaSwapped: onMediaSwapped,
      onAnalysisOverlayPanelToggle: onAnalysisOverlayPanelToggle,
      onAnalysisOverlayTypeChanged: onAnalysisOverlayTypeChanged,
      onAnalysisOverlayLayersChanged: onAnalysisOverlayLayersChanged,
      onAnalysisOverlayOpacityChanged: onAnalysisOverlayOpacityChanged,
      onRemoveClicked: onRemoveClicked,
    );
  }
}

class _MediaHeaderBarWithCache extends StatefulWidget {
  final List<TrackEntry> entries;
  final AnalysisToolbarDataSource analysisDataSource;
  final Key? analysisOverlayButtonKey;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final Future<void> Function() onAnalysisOverlayPanelToggle;
  final ValueChanged<AnalysisOverlayType> onAnalysisOverlayTypeChanged;
  final ValueChanged<Set<AnalysisOverlayLayer>> onAnalysisOverlayLayersChanged;
  final ValueChanged<double> onAnalysisOverlayOpacityChanged;
  final void Function(int slotIndex) onRemoveClicked;

  const _MediaHeaderBarWithCache({
    required this.entries,
    required this.analysisDataSource,
    this.analysisOverlayButtonKey,
    required this.onMediaSwapped,
    required this.onAnalysisOverlayPanelToggle,
    required this.onAnalysisOverlayTypeChanged,
    required this.onAnalysisOverlayLayersChanged,
    required this.onAnalysisOverlayOpacityChanged,
    required this.onRemoveClicked,
  });

  @override
  State<_MediaHeaderBarWithCache> createState() =>
      _MediaHeaderBarWithCacheState();
}

class _MediaHeaderBarWithCacheState extends State<_MediaHeaderBarWithCache> {
  @override
  Widget build(BuildContext context) {
    if (widget.entries.isEmpty) return const SizedBox.shrink();
    return SizedBox(
      height: 32,
      child: Align(
        alignment: Alignment.bottomCenter,
        child: SizedBox(
          height: 28,
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 4),
            child: Row(
              children: [
                for (int i = 0; i < widget.entries.length; i++) ...[
                  if (i > 0) const SizedBox(width: 4),
                  Expanded(
                    child: _MediaHeader(
                      slotIndex: i,
                      entries: widget.entries,
                      analysisDataSource: widget.analysisDataSource,
                      analysisOverlayButtonKey: i == 0
                          ? widget.analysisOverlayButtonKey
                          : null,
                      showOverlayPanelButton: i == 0,
                      onMediaSwapped: widget.onMediaSwapped,
                      onAnalysisOverlayPanelToggle:
                          widget.onAnalysisOverlayPanelToggle,
                      onAnalysisOverlayTypeChanged:
                          widget.onAnalysisOverlayTypeChanged,
                      onAnalysisOverlayLayersChanged:
                          widget.onAnalysisOverlayLayersChanged,
                      onAnalysisOverlayOpacityChanged:
                          widget.onAnalysisOverlayOpacityChanged,
                      onRemoveClicked: widget.onRemoveClicked,
                    ),
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}

/// Single track header with source combo box and action buttons.
class _MediaHeader extends StatelessWidget {
  final int slotIndex;
  final List<TrackEntry> entries;
  final AnalysisToolbarDataSource analysisDataSource;
  final Key? analysisOverlayButtonKey;
  final bool showOverlayPanelButton;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final Future<void> Function() onAnalysisOverlayPanelToggle;
  final ValueChanged<AnalysisOverlayType> onAnalysisOverlayTypeChanged;
  final ValueChanged<Set<AnalysisOverlayLayer>> onAnalysisOverlayLayersChanged;
  final ValueChanged<double> onAnalysisOverlayOpacityChanged;
  final void Function(int slotIndex) onRemoveClicked;

  const _MediaHeader({
    required this.slotIndex,
    required this.entries,
    required this.analysisDataSource,
    this.analysisOverlayButtonKey,
    required this.showOverlayPanelButton,
    required this.onMediaSwapped,
    required this.onAnalysisOverlayPanelToggle,
    required this.onAnalysisOverlayTypeChanged,
    required this.onAnalysisOverlayLayersChanged,
    required this.onAnalysisOverlayOpacityChanged,
    required this.onRemoveClicked,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Container(
      height: 28,
      decoration: BoxDecoration(
        color: theme.colorScheme.surfaceContainerHighest.withValues(alpha: 0.5),
        borderRadius: BorderRadius.circular(6),
      ),
      child: Row(
        children: [
          if (showOverlayPanelButton)
            _HeaderOverlayPanelButton(
              key: analysisOverlayButtonKey,
              entries: entries,
              dataSource: analysisDataSource,
              onToggle: onAnalysisOverlayPanelToggle,
              onTypeChanged: onAnalysisOverlayTypeChanged,
              onLayersChanged: onAnalysisOverlayLayersChanged,
              onOpacityChanged: onAnalysisOverlayOpacityChanged,
            ),
          Expanded(
            child: _SourceComboBox(
              entries: entries,
              currentIndex: slotIndex,
              onChanged: (targetIndex) {
                if (targetIndex != slotIndex) {
                  onMediaSwapped(slotIndex, targetIndex);
                }
              },
            ),
          ),
          SizedBox(
            width: 28,
            height: 28,
            child: IconButton(
              onPressed: () => onRemoveClicked(slotIndex),
              icon: const Icon(Icons.close, size: 14),
              tooltip: AppLocalizations.of(context)!.removeTrack,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints.tightFor(width: 28, height: 28),
              style: _removeTrackButtonStyle(theme.colorScheme, 6),
            ),
          ),
        ],
      ),
    );
  }
}

ButtonStyle _removeTrackButtonStyle(ColorScheme colorScheme, double radius) {
  final warningStates = {
    WidgetState.hovered,
    WidgetState.focused,
    WidgetState.pressed,
  };
  return ButtonStyle(
    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
    shape: WidgetStatePropertyAll(
      RoundedRectangleBorder(borderRadius: BorderRadius.circular(radius)),
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

class _HeaderOverlayPanelButton extends StatefulWidget {
  final List<TrackEntry> entries;
  final AnalysisToolbarDataSource dataSource;
  final Future<void> Function() onToggle;
  final ValueChanged<AnalysisOverlayType> onTypeChanged;
  final ValueChanged<Set<AnalysisOverlayLayer>> onLayersChanged;
  final ValueChanged<double> onOpacityChanged;

  const _HeaderOverlayPanelButton({
    super.key,
    required this.entries,
    required this.dataSource,
    required this.onToggle,
    required this.onTypeChanged,
    required this.onLayersChanged,
    required this.onOpacityChanged,
  });

  @override
  State<_HeaderOverlayPanelButton> createState() =>
      _HeaderOverlayPanelButtonState();
}

class _HeaderOverlayPanelButtonState extends State<_HeaderOverlayPanelButton>
    with SingleTickerProviderStateMixin {
  final LayerLink _layerLink = LayerLink();
  OverlayEntry? _overlayEntry;
  Timer? _hideTimer;
  bool _hoveringButton = false;
  bool _hoveringPanel = false;
  String? _lastDataSourceSignature;
  late final AnimationController _panelAnimationController;
  late final Animation<double> _panelOpacity;

  @override
  void initState() {
    super.initState();
    widget.dataSource.addListener(_handleDataSourceChanged);
    _lastDataSourceSignature = _dataSourceSignature();
    _panelAnimationController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 130),
      reverseDuration: const Duration(milliseconds: 110),
    );
    _panelOpacity = CurvedAnimation(
      parent: _panelAnimationController,
      curve: Curves.easeOutCubic,
      reverseCurve: Curves.easeInCubic,
    );
  }

  @override
  void didUpdateWidget(covariant _HeaderOverlayPanelButton oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.dataSource != widget.dataSource) {
      oldWidget.dataSource.removeListener(_handleDataSourceChanged);
      widget.dataSource.addListener(_handleDataSourceChanged);
      _lastDataSourceSignature = _dataSourceSignature();
    }
    if (!widget.dataSource.overlayPanelVisible) {
      _removePanel();
    } else {
      _overlayEntry?.markNeedsBuild();
    }
  }

  @override
  void dispose() {
    _hideTimer?.cancel();
    widget.dataSource.removeListener(_handleDataSourceChanged);
    _removePanel();
    _panelAnimationController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final active = widget.dataSource.overlayPanelVisible;
    final working = _isAnalysisWorking(widget.dataSource.state);
    final enabled = !working;
    final tooltip = active
        ? AppLocalizations.of(context)!.analysisOverlayDeactivate
        : AppLocalizations.of(context)!.analysisOverlayActivate;
    return MouseRegion(
      onEnter: (_) {
        _hoveringButton = true;
        if (active) _showPanel();
      },
      onExit: (_) {
        _hoveringButton = false;
        _scheduleHidePanel();
      },
      child: CompositedTransformTarget(
        link: _layerLink,
        child: SizedBox(
          width: 28,
          height: 28,
          child: Semantics(
            button: true,
            toggled: active,
            enabled: enabled,
            label: tooltip,
            onTap: enabled ? () => unawaited(_handlePressed()) : null,
            child: Tooltip(
              message: tooltip,
              excludeFromSemantics: true,
              child: ExcludeSemantics(
                child: IconButton(
                  onPressed: enabled ? () => unawaited(_handlePressed()) : null,
                  icon: working
                      ? const SizedBox(
                          width: 13,
                          height: 13,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : const Icon(Icons.grid_on, size: 14),
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
                    backgroundColor: WidgetStateProperty.resolveWith((states) {
                      if (!enabled) return Colors.transparent;
                      if (active) {
                        return colorScheme.primary.withValues(alpha: 0.22);
                      }
                      if (states.contains(WidgetState.hovered) ||
                          states.contains(WidgetState.focused)) {
                        return colorScheme.primary.withValues(alpha: 0.14);
                      }
                      return Colors.transparent;
                    }),
                    foregroundColor: WidgetStateProperty.resolveWith((states) {
                      if (!enabled) {
                        return colorScheme.onSurfaceVariant.withValues(
                          alpha: 0.34,
                        );
                      }
                      return active
                          ? colorScheme.primary
                          : colorScheme.onSurfaceVariant;
                    }),
                    overlayColor: const WidgetStatePropertyAll(
                      Colors.transparent,
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }

  Future<void> _handlePressed() async {
    await widget.onToggle();
    if (!mounted) return;
    if (widget.dataSource.overlayPanelVisible) {
      _showPanel();
    } else {
      _removePanel();
    }
  }

  void _handleDataSourceChanged() {
    if (!mounted) return;
    final signature = _dataSourceSignature();
    if (signature == _lastDataSourceSignature) return;
    _lastDataSourceSignature = signature;
    setState(() {});
    if (!widget.dataSource.overlayPanelVisible) {
      _removePanel();
      return;
    }
    _overlayEntry?.markNeedsBuild();
  }

  void _showPanel() {
    if (!widget.dataSource.overlayPanelVisible) return;
    _hideTimer?.cancel();
    if (_overlayEntry != null) {
      _overlayEntry!.markNeedsBuild();
      _panelAnimationController.forward();
      return;
    }
    final overlay = Overlay.of(context);
    _overlayEntry = OverlayEntry(
      builder: (context) {
        const panelMargin = AnalysisOverlayControlBar.margin;
        final screenWidth = MediaQuery.sizeOf(context).width;
        final targetRenderBox = this.context.findRenderObject();
        final targetLeft = targetRenderBox is RenderBox
            ? targetRenderBox.localToGlobal(Offset.zero).dx
            : 0.0;
        final maxPanelContentWidth = math.max(
          1.0,
          screenWidth - targetLeft - panelMargin,
        );
        final panelWidth = math.min(
          maxPanelContentWidth,
          math.max(420.0, widget.entries.length * 500.0),
        );
        return ExcludeSemantics(
          child: CompositedTransformFollower(
            link: _layerLink,
            targetAnchor: Alignment.topLeft,
            followerAnchor: Alignment.bottomLeft,
            offset: const Offset(0, -panelMargin),
            showWhenUnlinked: false,
            child: FadeTransition(
              opacity: _panelOpacity,
              child: UnconstrainedBox(
                alignment: Alignment.bottomLeft,
                child: MouseRegion(
                  onEnter: (_) {
                    _hoveringPanel = true;
                    _hideTimer?.cancel();
                  },
                  onExit: (_) {
                    _hoveringPanel = false;
                    _scheduleHidePanel();
                  },
                  child: Material(
                    color: Colors.transparent,
                    child: SizedBox(
                      width: panelWidth,
                      child: AnalysisOverlayControlBar(
                        entries: widget.entries,
                        dataSource: widget.dataSource,
                        onTypeChanged: widget.onTypeChanged,
                        onLayersChanged: widget.onLayersChanged,
                        onOpacityChanged: widget.onOpacityChanged,
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
        );
      },
    );
    overlay.insert(_overlayEntry!);
    _panelAnimationController.forward(from: 0);
  }

  void _scheduleHidePanel() {
    _hideTimer?.cancel();
    _hideTimer = Timer(const Duration(milliseconds: 180), () {
      if (_hoveringButton || _hoveringPanel) return;
      unawaited(_fadeOutPanel());
    });
  }

  Future<void> _fadeOutPanel() async {
    await _panelAnimationController.reverse();
    if (!mounted) return;
    if (_hoveringButton || _hoveringPanel) {
      await _panelAnimationController.forward();
      return;
    }
    _removePanel();
  }

  void _removePanel() {
    _overlayEntry?.remove();
    _overlayEntry = null;
    if (_panelAnimationController.isAnimating ||
        _panelAnimationController.value != 0) {
      _panelAnimationController.value = 0;
    }
  }

  bool _isAnalysisWorking(AnalysisState state) {
    return state == AnalysisState.computingHash ||
        state == AnalysisState.generating ||
        state == AnalysisState.loading;
  }

  String _dataSourceSignature() {
    final activeTrackIds = widget.dataSource.activeOverlayTrackFileIds.toList()
      ..sort();
    final config = widget.dataSource.overlayConfig;
    final layers = config.layers.map((layer) => layer.name).toList()..sort();
    return [
      widget.dataSource.state.name,
      widget.dataSource.overlayPanelVisible,
      activeTrackIds.join('|'),
      config.type.name,
      layers.join('|'),
      config.opacity.toStringAsFixed(3),
    ].join(';');
  }
}

/// Source selector combo box following ZoomComboBox's MenuAnchor pattern.
class _SourceComboBox extends StatelessWidget {
  final List<TrackEntry> entries;
  final int currentIndex;
  final ValueChanged<int> onChanged;

  const _SourceComboBox({
    required this.entries,
    required this.currentIndex,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return AppMenuCombo<int>(
      height: 28,
      value: currentIndex,
      items: [for (var i = 0; i < entries.length; i++) i],
      labelFor: (i) => i < entries.length ? entries[i].fileName : '',
      onChanged: onChanged,
      buttonPadding: const EdgeInsets.symmetric(horizontal: 4),
      borderRadius: BorderRadius.circular(6),
      textStyle: theme.textTheme.bodySmall,
      menuTextStyle: theme.textTheme.bodySmall,
      iconSize: 16,
    );
  }
}
