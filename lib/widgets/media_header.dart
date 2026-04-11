import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';
import '../track_manager.dart';

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
      padding: const EdgeInsets.only(left: 4),
      child: Row(
        children: [
          // Source combo box (takes remaining space)
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
          // Remove button
          SizedBox(
            width: 28,
            height: 28,
            child: IconButton(
              onPressed: () => onRemoveClicked(slotIndex),
              icon: Icon(Icons.close, size: 14, color: theme.colorScheme.onSurfaceVariant),
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
    return MenuAnchor(
      alignmentOffset: const Offset(0, 4),
      style: MenuStyle(
        backgroundColor: WidgetStatePropertyAll(
          theme.colorScheme.surfaceContainerHigh,
        ),
        shape: WidgetStatePropertyAll(
          RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
        ),
        padding: const WidgetStatePropertyAll(EdgeInsets.symmetric(vertical: 4)),
      ),
      menuChildren: List.generate(entries.length, (i) {
        final selected = i == currentIndex;
        return MenuItemButton(
          leadingIcon: selected
              ? Icon(Icons.check, size: 16, color: theme.colorScheme.primary)
              : const SizedBox(width: 16),
          requestFocusOnHover: false,
          style: ButtonStyle(
            padding: WidgetStatePropertyAll(
              EdgeInsets.only(left: selected ? 8.0 : 12.0, right: 16),
            ),
          ),
          onPressed: () => onChanged(i),
          child: Text(
            entries[i].fileName,
            style: theme.textTheme.bodySmall?.copyWith(
              color: selected ? theme.colorScheme.primary : null,
            ),
            overflow: TextOverflow.ellipsis,
          ),
        );
      }),
      builder: (context, controller, child) {
        return Material(
          color: Colors.transparent,
          child: InkWell(
            onTap: () {
              if (controller.isOpen) {
                controller.close();
              } else {
                controller.open();
              }
            },
            borderRadius: BorderRadius.circular(4),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 4),
              child: Row(
                children: [
                  Expanded(
                    child: Text(
                      currentIndex < entries.length
                          ? entries[currentIndex].fileName
                          : '',
                      style: theme.textTheme.bodySmall,
                      overflow: TextOverflow.ellipsis,
                      maxLines: 1,
                    ),
                  ),
                  Icon(
                    Icons.arrow_drop_down,
                    size: 16,
                    color: theme.iconTheme.color,
                  ),
                ],
              ),
            ),
          ),
        );
      },
    );
  }
}
