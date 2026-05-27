import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';
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
  final bool analysisOverlayEnabled;
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
    this.analysisOverlayEnabled = true,
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
      analysisOverlayEnabled: analysisOverlayEnabled,
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

class MediaHeaderOverlayPanelHost extends StatefulWidget {
  final List<TrackEntry> entries;
  final AnalysisToolbarDataSource dataSource;
  final ValueChanged<AnalysisOverlayType> onTypeChanged;
  final ValueChanged<Set<AnalysisOverlayLayer>> onLayersChanged;
  final ValueChanged<double> onOpacityChanged;
  final Widget child;

  const MediaHeaderOverlayPanelHost({
    super.key,
    required this.entries,
    required this.dataSource,
    required this.onTypeChanged,
    required this.onLayersChanged,
    required this.onOpacityChanged,
    required this.child,
  });

  static MediaHeaderOverlayPanelHostState? maybeOf(BuildContext context) {
    final scope = context
        .getElementForInheritedWidgetOfExactType<
          _MediaHeaderOverlayPanelScope
        >()
        ?.widget;
    return scope is _MediaHeaderOverlayPanelScope ? scope.state : null;
  }

  @override
  State<MediaHeaderOverlayPanelHost> createState() =>
      MediaHeaderOverlayPanelHostState();
}

class _MediaHeaderOverlayPanelScope extends InheritedWidget {
  final MediaHeaderOverlayPanelHostState state;

  const _MediaHeaderOverlayPanelScope({
    required this.state,
    required super.child,
  });

  @override
  bool updateShouldNotify(covariant _MediaHeaderOverlayPanelScope oldWidget) {
    return oldWidget.state != state;
  }
}

enum _HeaderOverlayPanelPhase { hidden, shown, closing }

class MediaHeaderOverlayPanelHostState
    extends State<MediaHeaderOverlayPanelHost>
    with SingleTickerProviderStateMixin {
  Timer? _hideTimer;
  Rect? _anchorRect;
  bool _hoveringButton = false;
  bool _hoveringPanel = false;
  _HeaderOverlayPanelPhase _panelPhase = _HeaderOverlayPanelPhase.hidden;
  Set<int> _cachedTrackFileIds = const {};
  Future<void>? _cacheRefreshInFlight;
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
  void didUpdateWidget(covariant MediaHeaderOverlayPanelHost oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.dataSource != widget.dataSource) {
      oldWidget.dataSource.removeListener(_handleDataSourceChanged);
      widget.dataSource.addListener(_handleDataSourceChanged);
      _lastDataSourceSignature = _dataSourceSignature();
    }
    if (widget.entries.isEmpty) {
      _removePanel();
    } else if (_isPanelVisible) {
      setState(() {});
    }
  }

  @override
  void dispose() {
    _hideTimer?.cancel();
    widget.dataSource.removeListener(_handleDataSourceChanged);
    _panelAnimationController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return _MediaHeaderOverlayPanelScope(
      state: this,
      child: LayoutBuilder(
        builder: (context, constraints) {
          return Stack(
            clipBehavior: Clip.none,
            children: [
              widget.child,
              if (_isPanelVisible && widget.entries.isNotEmpty)
                _buildPanel(context, constraints),
            ],
          );
        },
      ),
    );
  }

  Widget _buildPanel(BuildContext context, BoxConstraints constraints) {
    final anchor = _anchorRect;
    if (anchor == null) return const SizedBox.shrink();
    const panelMargin = AnalysisOverlayControlBar.margin;
    final panelWidth = math.min(
      math.max(1.0, constraints.maxWidth - anchor.left - panelMargin),
      math.max(420.0, widget.entries.length * 500.0),
    );
    final left = anchor.left
        .clamp(0.0, math.max(0.0, constraints.maxWidth - panelWidth))
        .toDouble();
    final bottom = math.max(
      0.0,
      constraints.maxHeight - anchor.top + panelMargin,
    );
    return Positioned(
      left: left,
      bottom: bottom,
      width: panelWidth,
      child: IgnorePointer(
        ignoring: _panelPhase == _HeaderOverlayPanelPhase.hidden,
        child: FadeTransition(
          opacity: _panelOpacity,
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
              child: AnalysisOverlayControlBar(
                entries: widget.entries,
                dataSource: widget.dataSource,
                panelActive: widget.dataSource.overlayPanelVisible,
                readyTrackFileIds: _cachedTrackFileIds,
                onTypeChanged: widget.onTypeChanged,
                onLayersChanged: widget.onLayersChanged,
                onOpacityChanged: widget.onOpacityChanged,
              ),
            ),
          ),
        ),
      ),
    );
  }

  void showFrom(BuildContext anchorContext) {
    if (widget.entries.isEmpty) return;
    _updateAnchor(anchorContext);
    _hoveringButton = true;
    _showPanel();
    unawaited(refreshCachedTrackFileIds());
  }

  void buttonExited() {
    _hoveringButton = false;
    _scheduleHidePanel();
  }

  Future<void> handleToggleCompleted({required bool stillHovering}) async {
    await refreshCachedTrackFileIds();
    if (!mounted) return;
    if (stillHovering || _hoveringPanel) {
      _showPanel();
    } else {
      _removePanel();
    }
  }

  void _updateAnchor(BuildContext anchorContext) {
    final hostBox = context.findRenderObject();
    final targetBox = anchorContext.findRenderObject();
    if (hostBox is! RenderBox || targetBox is! RenderBox) return;
    final globalTopLeft = targetBox.localToGlobal(Offset.zero);
    final localTopLeft = hostBox.globalToLocal(globalTopLeft);
    _anchorRect = localTopLeft & targetBox.size;
  }

  void _showPanel() {
    _hideTimer?.cancel();
    setState(() {
      _panelPhase = _HeaderOverlayPanelPhase.shown;
    });
    _panelAnimationController.forward();
  }

  void _scheduleHidePanel() {
    _hideTimer?.cancel();
    _hideTimer = Timer(const Duration(milliseconds: 180), () {
      if (_hoveringButton || _hoveringPanel) return;
      unawaited(_fadeOutPanel());
    });
  }

  Future<void> _fadeOutPanel() async {
    if (!_isPanelVisible) return;
    setState(() {
      _panelPhase = _HeaderOverlayPanelPhase.closing;
    });
    await _panelAnimationController.reverse();
    if (!mounted) return;
    if (_hoveringButton || _hoveringPanel) {
      setState(() {
        _panelPhase = _HeaderOverlayPanelPhase.shown;
      });
      await _panelAnimationController.forward();
      return;
    }
    _removePanel();
  }

  void _removePanel() {
    _hideTimer?.cancel();
    _hoveringButton = false;
    _hoveringPanel = false;
    if (_panelAnimationController.isAnimating ||
        _panelAnimationController.value != 0) {
      _panelAnimationController.value = 0;
    }
    if (_panelPhase == _HeaderOverlayPanelPhase.hidden && _anchorRect == null) {
      return;
    }
    setState(() {
      _panelPhase = _HeaderOverlayPanelPhase.hidden;
      _anchorRect = null;
    });
  }

  bool get _isPanelVisible =>
      _panelPhase == _HeaderOverlayPanelPhase.shown ||
      _panelPhase == _HeaderOverlayPanelPhase.closing;

  Future<void> refreshCachedTrackFileIds() async {
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
          setState(() {
            _cachedTrackFileIds = ready;
          });
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
    if (_isPanelVisible) setState(() {});
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

class _MediaHeaderBarWithCache extends StatefulWidget {
  final List<TrackEntry> entries;
  final bool analysisOverlayEnabled;
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
    required this.analysisOverlayEnabled,
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
                      showOverlayPanelButton:
                          widget.analysisOverlayEnabled && i == 0,
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

class _HeaderOverlayPanelButtonState extends State<_HeaderOverlayPanelButton> {
  bool _hoveringButton = false;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final active = widget.dataSource.overlayPanelVisible;
    final working = _isAnalysisWorking(widget.dataSource.state);
    final tooltip = active
        ? AppLocalizations.of(context)!.analysisOverlayDeactivate
        : AppLocalizations.of(context)!.analysisOverlayActivate;
    return MouseRegion(
      onEnter: (_) {
        setState(() {
          _hoveringButton = true;
        });
        MediaHeaderOverlayPanelHost.maybeOf(context)?.showFrom(context);
      },
      onExit: (_) {
        setState(() {
          _hoveringButton = false;
        });
        MediaHeaderOverlayPanelHost.maybeOf(context)?.buttonExited();
      },
      child: SizedBox(
        width: 28,
        height: 28,
        child: Tooltip(
          message: tooltip,
          excludeFromSemantics: true,
          child: IconButton(
            onPressed: working ? null : () => unawaited(_handlePressed()),
            icon: working
                ? const SizedBox(
                    width: 13,
                    height: 13,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : const Icon(Icons.grid_on, size: 14),
            padding: EdgeInsets.zero,
            constraints: const BoxConstraints.tightFor(width: 28, height: 28),
            style: ButtonStyle(
              tapTargetSize: MaterialTapTargetSize.shrinkWrap,
              shape: WidgetStatePropertyAll(
                RoundedRectangleBorder(borderRadius: BorderRadius.circular(6)),
              ),
              backgroundColor: WidgetStateProperty.resolveWith((states) {
                if (working) return Colors.transparent;
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
                if (working) {
                  return colorScheme.onSurfaceVariant.withValues(alpha: 0.34);
                }
                return active
                    ? colorScheme.primary
                    : colorScheme.onSurfaceVariant;
              }),
              overlayColor: const WidgetStatePropertyAll(Colors.transparent),
            ),
          ),
        ),
      ),
    );
  }

  Future<void> _handlePressed() async {
    if (_isAnalysisWorking(widget.dataSource.state)) return;
    final host = MediaHeaderOverlayPanelHost.maybeOf(context);
    await widget.onToggle();
    if (!mounted) return;
    await host?.handleToggleCompleted(stillHovering: _hoveringButton);
  }

  bool _isAnalysisWorking(AnalysisState state) {
    return state == AnalysisState.computingHash ||
        state == AnalysisState.generating;
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
