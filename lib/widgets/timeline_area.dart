import 'package:flutter/material.dart';
import '../track_manager.dart';
import 'track_row.dart';

/// Timeline track list area matching PySide6 TimelineArea.
/// Max height: 40% of parent height. Each TrackRow is 40px.
/// Supports drag-to-reorder via [ReorderableListView].
/// Dragging any row's divider resizes all rows synchronously.
class TimelineArea extends StatefulWidget {
  final TrackManager trackManager;
  final int currentPtsUs;
  final void Function(int oldIndex, int newIndex) onReorder;
  final void Function(int slot, int offsetMs) onOffsetChanged;
  final ValueChanged<int> onRemoveTrack;
  final Map<int, int> syncOffsets;

  const TimelineArea({
    super.key,
    required this.trackManager,
    this.currentPtsUs = 0,
    required this.onReorder,
    required this.onOffsetChanged,
    required this.onRemoveTrack,
    this.syncOffsets = const {},
  });

  @override
  State<TimelineArea> createState() => _TimelineAreaState();
}

class _TimelineAreaState extends State<TimelineArea> {
  double _controlsWidth = 320;

  void _onControlsWidthChanged(double width) {
    setState(() {
      _controlsWidth = width;
    });
  }

  @override
  Widget build(BuildContext context) {
    final entries = widget.trackManager.entries;

    // Compute max duration across all tracks
    int maxDurationUs = 0;
    for (final e in entries) {
      if (e.info.durationUs > maxDurationUs) {
        maxDurationUs = e.info.durationUs;
      }
    }

    return LayoutBuilder(builder: (context, constraints) {
      final maxHeight = constraints.maxHeight;
      final targetHeight =
          (widget.trackManager.count * 40.0).clamp(0.0, maxHeight);
      return SizedBox(
        height: targetHeight,
        child: ReorderableListView.builder(
          buildDefaultDragHandles: false,
          itemCount: widget.trackManager.count,
          onReorder: widget.onReorder,
          itemBuilder: (context, index) {
            final entry = widget.trackManager.entries[index];
            final trackDuration = entry.info.durationUs;

            // Duration ratio: how long this clip is relative to the longest
            final durationRatio =
                maxDurationUs > 0 ? trackDuration / maxDurationUs : 1.0;

            // Per-track clamped playhead: cap at track's own duration
            double playheadPosition = 0.0;
            if (trackDuration > 0) {
              playheadPosition =
                  (widget.currentPtsUs / trackDuration).clamp(0.0, 1.0);
            }

            return TrackRow(
              key: ValueKey(entry.fileId),
              track: entry.info,
              index: index,
              playheadPosition: playheadPosition,
              durationRatio: durationRatio,
              onRemove: () => widget.onRemoveTrack(entry.fileId),
              onOffsetChanged: (delta) => widget.onOffsetChanged(
                  entry.slot, (widget.syncOffsets[entry.slot] ?? 0) + delta),
              syncOffsetMs: widget.syncOffsets[entry.slot] ?? 0,
              controlsWidth: _controlsWidth,
              onControlsWidthChanged: _onControlsWidthChanged,
            );
          },
        ),
      );
    });
  }
}
