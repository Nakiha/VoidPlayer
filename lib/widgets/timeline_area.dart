import 'package:flutter/material.dart';
import '../track_manager.dart';
import 'track_row.dart';

/// Timeline track list area matching PySide6 TimelineArea.
/// Max height: 40% of parent height. Each TrackRow is 40px.
/// Supports drag-to-reorder via [ReorderableListView].
class TimelineArea extends StatelessWidget {
  final TrackManager trackManager;
  final double playheadPosition; // 0.0 - 1.0
  final Map<int, bool> visibility;
  final Map<int, bool> muted;
  final Map<int, int> syncOffsets;
  final ValueChanged<int> onRemoveTrack;
  final void Function(int oldIndex, int newIndex) onReorder;
  final void Function(int slot, bool visible) onToggleVisibility;
  final void Function(int slot, bool muted) onToggleMute;
  final void Function(int slot, int offsetMs) onOffsetChanged;

  const TimelineArea({
    super.key,
    required this.trackManager,
    this.playheadPosition = 0.0,
    this.visibility = const {},
    this.muted = const {},
    this.syncOffsets = const {},
    required this.onRemoveTrack,
    required this.onReorder,
    required this.onToggleVisibility,
    required this.onToggleMute,
    required this.onOffsetChanged,
  });

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, constraints) {
      final maxHeight = constraints.maxHeight;
      final targetHeight = (trackManager.count * 40.0).clamp(0.0, maxHeight);
      return SizedBox(
        height: targetHeight,
        child: ReorderableListView.builder(
          buildDefaultDragHandles: false,
          itemCount: trackManager.count,
          onReorder: onReorder,
          itemBuilder: (context, index) {
            final entry = trackManager.entries[index];
            return TrackRow(
              key: ValueKey(entry.fileId),
              track: entry.info,
              index: index,
              playheadPosition: playheadPosition,
              onRemove: () => onRemoveTrack(entry.fileId),
              onToggleVisibility: () =>
                  onToggleVisibility(entry.slot, !(visibility[entry.slot] ?? true)),
              onToggleMute: () =>
                  onToggleMute(entry.slot, !(muted[entry.slot] ?? false)),
              onOffsetChanged: (delta) => onOffsetChanged(
                  entry.slot, (syncOffsets[entry.slot] ?? 0) + delta),
              isVisible: visibility[entry.slot] ?? true,
              isMuted: muted[entry.slot] ?? false,
              syncOffsetMs: syncOffsets[entry.slot] ?? 0,
            );
          },
        ),
      );
    });
  }
}
