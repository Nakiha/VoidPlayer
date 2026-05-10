import 'dart:async';

import 'package:flutter/material.dart';
import '../analysis/analysis_manager.dart';
import '../analysis/analysis_toolbar_data_source.dart';
import '../l10n/app_localizations.dart';
import '../track_manager.dart';
import 'app_menu_combo.dart';

/// Bar of per-track media headers, placed between viewport and controls bar.
///
/// Each header shows a combo box for switching media sources and a remove button,
/// matching the PySide6 `MediaHeader`.
class MediaHeaderBar extends StatelessWidget {
  final List<TrackEntry> entries;
  final AnalysisToolbarDataSource analysisDataSource;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final Future<void> Function() onAnalysisOverlayPanelToggle;
  final void Function(int slotIndex) onRemoveClicked;

  const MediaHeaderBar({
    super.key,
    required this.entries,
    required this.analysisDataSource,
    required this.onMediaSwapped,
    required this.onAnalysisOverlayPanelToggle,
    required this.onRemoveClicked,
  });

  @override
  Widget build(BuildContext context) {
    return _MediaHeaderBarWithCache(
      entries: entries,
      analysisDataSource: analysisDataSource,
      onMediaSwapped: onMediaSwapped,
      onAnalysisOverlayPanelToggle: onAnalysisOverlayPanelToggle,
      onRemoveClicked: onRemoveClicked,
    );
  }
}

class _MediaHeaderBarWithCache extends StatefulWidget {
  final List<TrackEntry> entries;
  final AnalysisToolbarDataSource analysisDataSource;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final Future<void> Function() onAnalysisOverlayPanelToggle;
  final void Function(int slotIndex) onRemoveClicked;

  const _MediaHeaderBarWithCache({
    required this.entries,
    required this.analysisDataSource,
    required this.onMediaSwapped,
    required this.onAnalysisOverlayPanelToggle,
    required this.onRemoveClicked,
  });

  @override
  State<_MediaHeaderBarWithCache> createState() =>
      _MediaHeaderBarWithCacheState();
}

class _MediaHeaderBarWithCacheState extends State<_MediaHeaderBarWithCache> {
  @override
  void initState() {
    super.initState();
    widget.analysisDataSource.addListener(_handleAnalysisChanged);
  }

  @override
  void didUpdateWidget(covariant _MediaHeaderBarWithCache oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.analysisDataSource != widget.analysisDataSource) {
      oldWidget.analysisDataSource.removeListener(_handleAnalysisChanged);
      widget.analysisDataSource.addListener(_handleAnalysisChanged);
    }
  }

  @override
  void dispose() {
    widget.analysisDataSource.removeListener(_handleAnalysisChanged);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (widget.entries.isEmpty) return const SizedBox.shrink();
    return Container(
      height: 32,
      padding: const EdgeInsets.symmetric(horizontal: 4),
      child: Align(
        alignment: Alignment.bottomCenter,
        child: SizedBox(
          width: double.infinity,
          height: 28,
          child: Row(
            children: [
              for (int i = 0; i < widget.entries.length; i++) ...[
                if (i > 0) const SizedBox(width: 4),
                Expanded(
                  child: _MediaHeader(
                    slotIndex: i,
                    entries: widget.entries,
                    showOverlayPanelButton: i == 0,
                    overlayPanelActive:
                        widget.analysisDataSource.activeOverlayHash != null,
                    overlayWorking: _isAnalysisWorking,
                    onMediaSwapped: widget.onMediaSwapped,
                    onAnalysisOverlayPanelToggle:
                        widget.onAnalysisOverlayPanelToggle,
                    onRemoveClicked: widget.onRemoveClicked,
                  ),
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }

  void _handleAnalysisChanged() {
    if (!mounted) return;
    setState(() {});
  }

  bool get _isAnalysisWorking {
    return widget.analysisDataSource.state == AnalysisState.computingHash ||
        widget.analysisDataSource.state == AnalysisState.generating ||
        widget.analysisDataSource.state == AnalysisState.loading;
  }
}

/// Single track header with source combo box and action buttons.
class _MediaHeader extends StatelessWidget {
  final int slotIndex;
  final List<TrackEntry> entries;
  final bool showOverlayPanelButton;
  final bool overlayPanelActive;
  final bool overlayWorking;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final Future<void> Function() onAnalysisOverlayPanelToggle;
  final void Function(int slotIndex) onRemoveClicked;

  const _MediaHeader({
    required this.slotIndex,
    required this.entries,
    required this.showOverlayPanelButton,
    required this.overlayPanelActive,
    required this.overlayWorking,
    required this.onMediaSwapped,
    required this.onAnalysisOverlayPanelToggle,
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
              active: overlayPanelActive,
              working: overlayWorking,
              onToggle: onAnalysisOverlayPanelToggle,
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
              icon: Icon(
                Icons.close,
                size: 14,
                color: theme.colorScheme.onSurfaceVariant,
              ),
              tooltip: AppLocalizations.of(context)!.removeTrack,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints.tightFor(width: 28, height: 28),
            ),
          ),
        ],
      ),
    );
  }
}

class _HeaderOverlayPanelButton extends StatelessWidget {
  final bool active;
  final bool working;
  final Future<void> Function() onToggle;

  const _HeaderOverlayPanelButton({
    required this.active,
    required this.working,
    required this.onToggle,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final enabled = !working;
    return SizedBox(
      width: 28,
      height: 28,
      child: IconButton(
        onPressed: enabled ? () => unawaited(onToggle()) : null,
        icon: working
            ? const SizedBox(
                width: 13,
                height: 13,
                child: CircularProgressIndicator(strokeWidth: 2),
              )
            : const Icon(Icons.grid_on, size: 14),
        tooltip: active
            ? AppLocalizations.of(context)!.analysisOverlayDeactivate
            : AppLocalizations.of(context)!.analysisOverlayActivate,
        padding: EdgeInsets.zero,
        constraints: const BoxConstraints.tightFor(width: 28, height: 28),
        style: ButtonStyle(
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          shape: WidgetStatePropertyAll(
            RoundedRectangleBorder(borderRadius: BorderRadius.circular(6)),
          ),
          backgroundColor: WidgetStateProperty.resolveWith((states) {
            if (!enabled) return Colors.transparent;
            if (active) return colorScheme.primary.withValues(alpha: 0.22);
            if (states.contains(WidgetState.hovered) ||
                states.contains(WidgetState.focused)) {
              return colorScheme.primary.withValues(alpha: 0.14);
            }
            return Colors.transparent;
          }),
          foregroundColor: WidgetStateProperty.resolveWith((states) {
            if (!enabled) {
              return colorScheme.onSurfaceVariant.withValues(alpha: 0.34);
            }
            return active ? colorScheme.primary : colorScheme.onSurfaceVariant;
          }),
          overlayColor: const WidgetStatePropertyAll(Colors.transparent),
        ),
      ),
    );
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
