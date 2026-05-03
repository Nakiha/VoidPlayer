import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';
import '../track_manager.dart';
import 'app_menu_combo.dart';

/// Bar of per-track media headers, placed between viewport and controls bar.
///
/// Each header shows a combo box for switching media sources and a remove button,
/// matching the PySide6 `MediaHeader`.
class MediaHeaderBar extends StatelessWidget {
  final List<TrackEntry> entries;
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final void Function(int slotIndex) onRemoveClicked;

  const MediaHeaderBar({
    super.key,
    required this.entries,
    required this.onMediaSwapped,
    required this.onRemoveClicked,
  });

  @override
  Widget build(BuildContext context) {
    if (entries.isEmpty) return const SizedBox.shrink();
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
              for (int i = 0; i < entries.length; i++) ...[
                if (i > 0) const SizedBox(width: 4),
                Expanded(
                  child: _MediaHeader(
                    slotIndex: i,
                    entries: entries,
                    onMediaSwapped: onMediaSwapped,
                    onRemoveClicked: onRemoveClicked,
                  ),
                ),
              ],
            ],
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
  final void Function(int slotIndex, int targetTrackIndex) onMediaSwapped;
  final void Function(int slotIndex) onRemoveClicked;

  const _MediaHeader({
    required this.slotIndex,
    required this.entries,
    required this.onMediaSwapped,
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
