import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';
import '../video_renderer_controller.dart';
import 'track_content.dart';

/// Drag handle with hover highlight and grab cursor.
class _DragHandle extends StatefulWidget {
  final int index;
  const _DragHandle({required this.index});

  @override
  State<_DragHandle> createState() => _DragHandleState();
}

class _DragHandleState extends State<_DragHandle> {
  bool _hovering = false;

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return MouseRegion(
      cursor: SystemMouseCursors.grab,
      onEnter: (_) => setState(() => _hovering = true),
      onExit: (_) => setState(() => _hovering = false),
      child: ReorderableDragStartListener(
        index: widget.index,
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 150),
          width: 28,
          height: 40,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            color: _hovering
                ? colorScheme.onSurface.withValues(alpha: 0.08)
                : Colors.transparent,
            borderRadius: BorderRadius.circular(4),
          ),
          child: Icon(
            Icons.drag_handle,
            size: 16,
            color: _hovering
                ? colorScheme.onSurface
                : colorScheme.onSurfaceVariant,
          ),
        ),
      ),
    );
  }
}

/// Single track row matching PySide6 TrackRow (40px height).
/// Horizontal split: drag handle + controls (320px) + track content (expanded).
class TrackRow extends StatelessWidget {
  final TrackInfo track;
  final int index; // display position index for ReorderableListView
  final double playheadPosition; // 0.0 - 1.0
  final VoidCallback onRemove;
  final ValueChanged<int> onOffsetChanged;
  final int syncOffsetMs;

  const TrackRow({
    super.key,
    required this.track,
    required this.index,
    this.playheadPosition = 0.0,
    required this.onRemove,
    required this.onOffsetChanged,
    this.syncOffsetMs = 0,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;

    return SizedBox(
      height: 40,
      child: Row(
        children: [
          // Drag handle with hover highlight
          _DragHandle(index: index),
          // Controls panel (320px)
          SizedBox(
            width: 320,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 4),
              child: Row(
                children: [
                  const SizedBox(width: 4),
                  // File name
                  Expanded(
                    child: Text(
                      _fileName(track.path),
                      style: Theme.of(context).textTheme.bodySmall,
                      overflow: TextOverflow.ellipsis,
                      maxLines: 1,
                    ),
                  ),
                  const SizedBox(width: 4),
                  // Offset controls
                  SizedBox(
                    width: 24,
                    height: 24,
                    child: IconButton(
                      onPressed: () => onOffsetChanged(-10),
                      icon: const Icon(Icons.remove, size: 14),
                      padding: EdgeInsets.zero,
                      constraints:
                          const BoxConstraints.tightFor(width: 24, height: 24),
                      tooltip: AppLocalizations.of(context)!.offsetBackward,
                    ),
                  ),
                  Text(
                    '${syncOffsetMs >= 0 ? '+' : ''}$syncOffsetMs',
                    style: Theme.of(context).textTheme.labelSmall,
                  ),
                  SizedBox(
                    width: 24,
                    height: 24,
                    child: IconButton(
                      onPressed: () => onOffsetChanged(10),
                      icon: const Icon(Icons.add, size: 14),
                      padding: EdgeInsets.zero,
                      constraints:
                          const BoxConstraints.tightFor(width: 24, height: 24),
                      tooltip: AppLocalizations.of(context)!.offsetForward,
                    ),
                  ),
                  // Remove button
                  SizedBox(
                    width: 28,
                    height: 28,
                    child: IconButton(
                      onPressed: onRemove,
                      icon: Icon(Icons.close, size: 16, color: colorScheme.error),
                      padding: EdgeInsets.zero,
                      constraints:
                          const BoxConstraints.tightFor(width: 28, height: 28),
                      tooltip: AppLocalizations.of(context)!.removeTrack,
                    ),
                  ),
                ],
              ),
            ),
          ),
          // Vertical divider (1px, matching HighlightSplitter handle)
          Container(width: 1, color: colorScheme.outlineVariant),
          // Track content (expanded)
          Expanded(
            child: TrackContent(
              playheadPosition: playheadPosition,
              clipColor: _trackColor(track.slot),
            ),
          ),
        ],
      ),
    );
  }

  String _fileName(String path) {
    final sep = path.contains('/') ? '/' : r'\';
    return path.split(sep).lastOrNull ?? path;
  }

  Color _trackColor(int slot) {
    const colors = [
      Colors.blue,
      Colors.orange,
      Colors.green,
      Colors.purple,
    ];
    return colors[slot % colors.length];
  }
}
