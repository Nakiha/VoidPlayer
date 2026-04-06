import 'package:flutter/material.dart';
import '../video_renderer_controller.dart';
import 'track_content.dart';

/// Single track row matching PySide6 TrackRow (40px height).
/// Horizontal split: controls (320px) + track content (expanded).
class TrackRow extends StatelessWidget {
  final TrackInfo track;
  final double playheadPosition; // 0.0 - 1.0
  final VoidCallback onRemove;
  final VoidCallback onToggleVisibility;
  final VoidCallback onToggleMute;
  final ValueChanged<int> onOffsetChanged;
  final bool isVisible;
  final bool isMuted;
  final int syncOffsetMs;

  const TrackRow({
    super.key,
    required this.track,
    this.playheadPosition = 0.0,
    required this.onRemove,
    required this.onToggleVisibility,
    required this.onToggleMute,
    required this.onOffsetChanged,
    this.isVisible = true,
    this.isMuted = false,
    this.syncOffsetMs = 0,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;

    return SizedBox(
      height: 40,
      child: Row(
        children: [
          // Controls panel (320px)
          SizedBox(
            width: 320,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 4),
              child: Row(
                children: [
                  // Visibility toggle
                  SizedBox(
                    width: 28,
                    height: 28,
                    child: IconButton(
                      onPressed: onToggleVisibility,
                      icon: Icon(
                        isVisible ? Icons.visibility : Icons.visibility_off,
                        size: 16,
                      ),
                      padding: EdgeInsets.zero,
                      constraints: const BoxConstraints.tightFor(width: 28, height: 28),
                    ),
                  ),
                  // Mute toggle
                  SizedBox(
                    width: 28,
                    height: 28,
                    child: IconButton(
                      onPressed: onToggleMute,
                      icon: Icon(
                        isMuted ? Icons.volume_off : Icons.volume_up,
                        size: 16,
                      ),
                      padding: EdgeInsets.zero,
                      constraints: const BoxConstraints.tightFor(width: 28, height: 28),
                    ),
                  ),
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
                      constraints: const BoxConstraints.tightFor(width: 24, height: 24),
                      tooltip: '-10ms',
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
                      constraints: const BoxConstraints.tightFor(width: 24, height: 24),
                      tooltip: '+10ms',
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
                      constraints: const BoxConstraints.tightFor(width: 28, height: 28),
                      tooltip: 'Remove Track',
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
