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

Key mediaHeaderRemoveButtonKey(int fileId) =>
    ValueKey('media-header-remove-$fileId');

/// Bar of per-track media headers, placed between viewport and controls bar.
///
/// Each header shows a combo box for switching media sources and a remove button,
/// matching the PySide6 `MediaHeader`.
class MediaHeaderBar extends StatelessWidget {
  final List<TrackEntry> entries;
  final bool analysisOverlayEnabled;
  final AnalysisToolbarDataSource analysisDataSource;
  final Key? analysisOverlayButtonKey;
  final bool canRemoveTrack;
  final bool canReorderTrack;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final void Function(int fileId) onRemoveClicked;

  const MediaHeaderBar({
    super.key,
    required this.entries,
    this.analysisOverlayEnabled = true,
    required this.analysisDataSource,
    this.analysisOverlayButtonKey,
    this.canRemoveTrack = true,
    this.canReorderTrack = true,
    required this.onMediaSwapped,
    required this.onRemoveClicked,
  });

  @override
  Widget build(BuildContext context) {
    return _MediaHeaderBarWithCache(
      entries: entries,
      analysisOverlayEnabled: analysisOverlayEnabled,
      analysisDataSource: analysisDataSource,
      analysisOverlayButtonKey: analysisOverlayButtonKey,
      canRemoveTrack: canRemoveTrack,
      canReorderTrack: canReorderTrack,
      onMediaSwapped: onMediaSwapped,
      onRemoveClicked: onRemoveClicked,
    );
  }
}

class MediaHeaderOverlayPanelHost extends StatefulWidget {
  final List<TrackEntry> entries;
  final AnalysisToolbarDataSource dataSource;
  final Future<void> Function() onOverlayActivate;
  final VoidCallback onOverlayDeactivate;
  final ValueChanged<AnalysisOverlayType> onTypeChanged;
  final ValueChanged<double> onOpacityChanged;
  final Widget child;

  const MediaHeaderOverlayPanelHost({
    super.key,
    required this.entries,
    required this.dataSource,
    required this.onOverlayActivate,
    required this.onOverlayDeactivate,
    required this.onTypeChanged,
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
  final GlobalKey _hostStackKey = GlobalKey();
  Rect? _anchorRect;
  BuildContext? _anchorContext;
  bool _anchorRefreshScheduled = false;
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
      _scheduleAnchorRefresh();
      unawaited(refreshCachedTrackFileIds());
      setState(() {});
    }
  }

  @override
  void dispose() {
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
            key: _hostStackKey,
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
    _scheduleAnchorRefresh();
    final anchor = _anchorRect;
    if (anchor == null) return const SizedBox.shrink();
    const panelMargin = AnalysisOverlayControlBar.margin;
    final rightInset = math.max(panelMargin, anchor.left);
    final panelWidth = math.max(
      1.0,
      constraints.maxWidth - anchor.left - rightInset,
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
          child: Material(
            color: Colors.transparent,
            child: AnalysisOverlayControlBar(
              dataSource: widget.dataSource,
              panelReady: _cachedTrackFileIds.isNotEmpty,
              panelActive: widget.dataSource.overlayPanelVisible,
              onTypeChanged: widget.onTypeChanged,
              onOpacityChanged: widget.onOpacityChanged,
              onActivateOverlay: widget.onOverlayActivate,
              onDeactivateOverlay: widget.onOverlayDeactivate,
            ),
          ),
        ),
      ),
    );
  }

  void toggleFrom(BuildContext anchorContext) {
    if (_isPanelVisible) {
      _removePanel();
      return;
    }
    showFrom(anchorContext);
  }

  void showFrom(BuildContext anchorContext) {
    if (widget.entries.isEmpty) return;
    _anchorContext = anchorContext;
    _updateAnchor(anchorContext);
    _showPanel();
    unawaited(refreshCachedTrackFileIds());
  }

  bool _updateAnchor(BuildContext anchorContext) {
    final hostBox = _hostStackKey.currentContext?.findRenderObject();
    final targetBox = anchorContext.findRenderObject();
    if (hostBox is! RenderBox || targetBox is! RenderBox) return false;
    final globalTopLeft = targetBox.localToGlobal(Offset.zero);
    final localTopLeft = hostBox.globalToLocal(globalTopLeft);
    final nextRect = localTopLeft & targetBox.size;
    if (_anchorRect == nextRect) return false;
    _anchorRect = nextRect;
    return true;
  }

  void _showPanel() {
    setState(() {
      _panelPhase = _HeaderOverlayPanelPhase.shown;
    });
    _panelAnimationController.forward();
  }

  void _removePanel() {
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
      _anchorContext = null;
    });
  }

  bool get _isPanelVisible =>
      _panelPhase == _HeaderOverlayPanelPhase.shown ||
      _panelPhase == _HeaderOverlayPanelPhase.closing;

  bool get isPanelVisible => _isPanelVisible;

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

  void _scheduleAnchorRefresh() {
    if (_anchorRefreshScheduled || !_isPanelVisible) return;
    final anchorContext = _anchorContext;
    if (anchorContext == null) return;
    _anchorRefreshScheduled = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _anchorRefreshScheduled = false;
      if (!mounted || !_isPanelVisible) return;
      if (_updateAnchor(anchorContext)) {
        setState(() {});
      }
    });
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

class _MediaHeaderBarWithCache extends StatefulWidget {
  final List<TrackEntry> entries;
  final bool analysisOverlayEnabled;
  final AnalysisToolbarDataSource analysisDataSource;
  final Key? analysisOverlayButtonKey;
  final bool canRemoveTrack;
  final bool canReorderTrack;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final void Function(int fileId) onRemoveClicked;

  const _MediaHeaderBarWithCache({
    required this.entries,
    required this.analysisOverlayEnabled,
    required this.analysisDataSource,
    this.analysisOverlayButtonKey,
    required this.canRemoveTrack,
    required this.canReorderTrack,
    required this.onMediaSwapped,
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
                      key: ValueKey('media-header-${widget.entries[i].fileId}'),
                      slotIndex: i,
                      entries: widget.entries,
                      analysisDataSource: widget.analysisDataSource,
                      analysisOverlayButtonKey: i == 0
                          ? widget.analysisOverlayButtonKey
                          : null,
                      showOverlayPanelButton:
                          widget.analysisOverlayEnabled && i == 0,
                      canRemoveTrack: widget.canRemoveTrack,
                      canReorderTrack: widget.canReorderTrack,
                      onMediaSwapped: widget.onMediaSwapped,
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
  final bool canRemoveTrack;
  final bool canReorderTrack;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final void Function(int fileId) onRemoveClicked;

  const _MediaHeader({
    super.key,
    required this.slotIndex,
    required this.entries,
    required this.analysisDataSource,
    this.analysisOverlayButtonKey,
    required this.showOverlayPanelButton,
    required this.canRemoveTrack,
    required this.canReorderTrack,
    required this.onMediaSwapped,
    required this.onRemoveClicked,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final entry = entries[slotIndex];
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
              dataSource: analysisDataSource,
            ),
          Expanded(
            child: Opacity(
              opacity: canReorderTrack ? 1.0 : 0.55,
              child: IgnorePointer(
                ignoring: !canReorderTrack,
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
            ),
          ),
          SizedBox(
            width: 28,
            height: 28,
            child: IconButton(
              key: mediaHeaderRemoveButtonKey(entry.fileId),
              onPressed: canRemoveTrack
                  ? () => onRemoveClicked(entry.fileId)
                  : null,
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
  final AnalysisToolbarDataSource dataSource;

  const _HeaderOverlayPanelButton({super.key, required this.dataSource});

  @override
  State<_HeaderOverlayPanelButton> createState() =>
      _HeaderOverlayPanelButtonState();
}

class _HeaderOverlayPanelButtonState extends State<_HeaderOverlayPanelButton> {
  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final host = MediaHeaderOverlayPanelHost.maybeOf(context);
    final active = host?.isPanelVisible ?? false;
    final working = _isAnalysisWorking(widget.dataSource.state);
    final tooltip = active
        ? AppLocalizations.of(context)!.analysisOverlayDeactivate
        : AppLocalizations.of(context)!.analysisOverlayActivate;
    return SizedBox(
      width: 28,
      height: 28,
      child: Tooltip(
        message: tooltip,
        excludeFromSemantics: true,
        child: IconButton(
          onPressed: () => host?.toggleFrom(context),
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
    );
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
