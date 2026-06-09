import 'package:flutter/material.dart';
import '../analysis/analysis_manager.dart';
import '../analysis/analysis_toolbar_data_source.dart';
import '../l10n/app_localizations.dart';
import '../track_manager.dart';
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
  final bool analysisOverlayControlsVisible;
  final AnalysisToolbarDataSource analysisDataSource;
  final Key? analysisOverlayButtonKey;
  final bool canRemoveTrack;
  final bool canReorderTrack;
  final VoidCallback? onAnalysisOverlayControlsToggle;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final void Function(int fileId) onRemoveClicked;

  const MediaHeaderBar({
    super.key,
    required this.entries,
    this.analysisOverlayEnabled = true,
    this.analysisOverlayControlsVisible = false,
    required this.analysisDataSource,
    this.analysisOverlayButtonKey,
    this.canRemoveTrack = true,
    this.canReorderTrack = true,
    this.onAnalysisOverlayControlsToggle,
    required this.onMediaSwapped,
    required this.onRemoveClicked,
  });

  @override
  Widget build(BuildContext context) {
    return _MediaHeaderBarWithCache(
      entries: entries,
      analysisOverlayEnabled: analysisOverlayEnabled,
      analysisOverlayControlsVisible: analysisOverlayControlsVisible,
      analysisDataSource: analysisDataSource,
      analysisOverlayButtonKey: analysisOverlayButtonKey,
      canRemoveTrack: canRemoveTrack,
      canReorderTrack: canReorderTrack,
      onAnalysisOverlayControlsToggle: onAnalysisOverlayControlsToggle,
      onMediaSwapped: onMediaSwapped,
      onRemoveClicked: onRemoveClicked,
    );
  }
}

class _MediaHeaderBarWithCache extends StatefulWidget {
  final List<TrackEntry> entries;
  final bool analysisOverlayEnabled;
  final bool analysisOverlayControlsVisible;
  final AnalysisToolbarDataSource analysisDataSource;
  final Key? analysisOverlayButtonKey;
  final bool canRemoveTrack;
  final bool canReorderTrack;
  final VoidCallback? onAnalysisOverlayControlsToggle;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final void Function(int fileId) onRemoveClicked;

  const _MediaHeaderBarWithCache({
    required this.entries,
    required this.analysisOverlayEnabled,
    required this.analysisOverlayControlsVisible,
    required this.analysisDataSource,
    this.analysisOverlayButtonKey,
    required this.canRemoveTrack,
    required this.canReorderTrack,
    this.onAnalysisOverlayControlsToggle,
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
                      analysisOverlayControlsVisible:
                          widget.analysisOverlayControlsVisible,
                      analysisOverlayButtonKey: i == 0
                          ? widget.analysisOverlayButtonKey
                          : null,
                      showOverlayPanelButton:
                          widget.analysisOverlayEnabled && i == 0,
                      canRemoveTrack: widget.canRemoveTrack,
                      canReorderTrack: widget.canReorderTrack,
                      onAnalysisOverlayControlsToggle:
                          widget.onAnalysisOverlayControlsToggle,
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
  final bool analysisOverlayControlsVisible;
  final Key? analysisOverlayButtonKey;
  final bool showOverlayPanelButton;
  final bool canRemoveTrack;
  final bool canReorderTrack;
  final VoidCallback? onAnalysisOverlayControlsToggle;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final void Function(int fileId) onRemoveClicked;

  const _MediaHeader({
    super.key,
    required this.slotIndex,
    required this.entries,
    required this.analysisDataSource,
    required this.analysisOverlayControlsVisible,
    this.analysisOverlayButtonKey,
    required this.showOverlayPanelButton,
    required this.canRemoveTrack,
    required this.canReorderTrack,
    required this.onAnalysisOverlayControlsToggle,
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
              controlsVisible: analysisOverlayControlsVisible,
              onPressed: onAnalysisOverlayControlsToggle,
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
  final bool controlsVisible;
  final VoidCallback? onPressed;

  const _HeaderOverlayPanelButton({
    super.key,
    required this.dataSource,
    required this.controlsVisible,
    required this.onPressed,
  });

  @override
  State<_HeaderOverlayPanelButton> createState() =>
      _HeaderOverlayPanelButtonState();
}

class _HeaderOverlayPanelButtonState extends State<_HeaderOverlayPanelButton> {
  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final active =
        widget.controlsVisible || widget.dataSource.overlayPanelVisible;
    final working = _isAnalysisWorking(widget.dataSource.state);
    final tooltip = widget.controlsVisible
        ? AppLocalizations.of(context)!.analysisOverlayControlsHide
        : AppLocalizations.of(context)!.analysisOverlayControlsShow;
    return SizedBox(
      width: 28,
      height: 28,
      child: Tooltip(
        message: tooltip,
        excludeFromSemantics: true,
        child: IconButton(
          onPressed: widget.onPressed,
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
