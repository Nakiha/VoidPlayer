import 'package:flutter/material.dart';
import '../track_manager.dart';
import 'track_row.dart';

/// Timeline track list area matching PySide6 TimelineArea.
/// Max height: 40% of parent height. Each TrackRow is 40px.
/// Supports drag-to-reorder via [ReorderableListView].
class TimelineArea extends StatelessWidget {
  final TrackManager trackManager;
  final double playheadPosition; // 0.0 - 1.0
  final Map<int, int> syncOffsets;
  final ValueChanged<int> onRemoveTrack;
  final void Function(int oldIndex, int newIndex) onReorder;
  final void Function(int slot, int offsetMs) onOffsetChanged;

  const TimelineArea({
    super.key,
    required this.trackManager,
    this.playheadPosition = 0.0,
    this.syncOffsets = const {},
    required this.onRemoveTrack,
    required this.onReorder,
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
              onOffsetChanged: (delta) => onOffsetChanged(
                  entry.slot, (syncOffsets[entry.slot] ?? 0) + delta),
              syncOffsetMs: syncOffsets[entry.slot] ?? 0,
            );
          },
        ),
      );
    });
  }
}
