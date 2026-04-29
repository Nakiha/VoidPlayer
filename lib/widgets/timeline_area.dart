import 'package:flutter/material.dart';
import '../track_manager.dart';
import 'track_row.dart';

/// Timeline track list area matching PySide6 TimelineArea.
/// Max height: 40% of parent height. Each TrackRow is 40px.
/// Supports drag-to-reorder via [ReorderableListView].
/// Dragging any row's divider resizes all rows synchronously.
class TimelineArea extends StatefulWidget {
  final List<TrackEntry> entries;
  final int currentPtsUs;
  final void Function(int oldIndex, int newIndex) onReorder;
  final void Function(int slot, int offsetMs) onOffsetChanged;
  final ValueChanged<int> onRemoveTrack;
  final Map<int, int> syncOffsets; // slot -> offset in microseconds
  final int maxEffectiveDurationUs;
  final int hoverPtsUs;
  final bool sliderHovering;
  final double controlsWidth;
  final ValueChanged<double> onControlsWidthChanged;
  final List<int> markerPtsUs;
  final bool loopRangeEnabled;
  final int loopStartUs;
  final int loopEndUs;

  const TimelineArea({
    super.key,
    required this.entries,
    this.currentPtsUs = 0,
    required this.onReorder,
    required this.onOffsetChanged,
    required this.onRemoveTrack,
    this.syncOffsets = const {},
    this.maxEffectiveDurationUs = 0,
    this.hoverPtsUs = 0,
    this.sliderHovering = false,
    this.controlsWidth = 320,
    required this.onControlsWidthChanged,
    this.markerPtsUs = const [],
    this.loopRangeEnabled = false,
    this.loopStartUs = 0,
    this.loopEndUs = 0,
  });

  @override
  State<TimelineArea> createState() => _TimelineAreaState();
}

class _TimelineAreaState extends State<TimelineArea> {
  @override
  Widget build(BuildContext context) {
    final maxEffectiveDurationUs = widget.maxEffectiveDurationUs;

    return LayoutBuilder(
      builder: (context, constraints) {
        final maxHeight = constraints.maxHeight;
        final targetHeight = (widget.entries.length * 40.0).clamp(
          0.0,
          maxHeight,
        );
        return SizedBox(
          height: targetHeight,
          child: ReorderableListView.builder(
            buildDefaultDragHandles: false,
            itemCount: widget.entries.length,
            onReorder: widget.onReorder,
            itemBuilder: (context, index) {
              final entry = widget.entries[index];
              final trackDuration = entry.info.durationUs;
              final offsetUs = widget.syncOffsets[entry.info.slot] ?? 0;

              // Clip ratio: original duration relative to max effective duration
              final clipRatio = maxEffectiveDurationUs > 0
                  ? (trackDuration / maxEffectiveDurationUs).clamp(0.0, 1.0)
                  : 1.0;

              // Offset ratio: where the clip block starts
              final offsetRatio = maxEffectiveDurationUs > 0
                  ? (offsetUs / maxEffectiveDurationUs).clamp(0.0, 1.0)
                  : 0.0;

              // Per-track playhead: global time → track internal time
              double playheadPosition = 0.0;
              if (trackDuration > 0) {
                final localTime = widget.currentPtsUs - offsetUs;
                playheadPosition = (localTime / trackDuration).clamp(0.0, 1.0);
              }

              return TrackRow(
                key: ValueKey(entry.fileId),
                track: entry.info,
                index: index,
                playheadPosition: playheadPosition,
                durationRatio: clipRatio,
                offsetRatio: offsetRatio,
                onRemove: () => widget.onRemoveTrack(entry.fileId),
                onOffsetChanged: (delta) =>
                    widget.onOffsetChanged(entry.info.slot, delta),
                syncOffsetMs: offsetUs ~/ 1000,
                controlsWidth: widget.controlsWidth,
                onControlsWidthChanged: widget.onControlsWidthChanged,
                hoverPtsUs: widget.hoverPtsUs,
                sliderHovering: widget.sliderHovering,
                trackDurationUs: trackDuration,
                offsetUs: offsetUs,
                maxEffectiveDurationUs: maxEffectiveDurationUs,
                markerPtsUs: widget.markerPtsUs,
                loopRangeEnabled: widget.loopRangeEnabled,
                loopStartUs: widget.loopStartUs,
                loopEndUs: widget.loopEndUs,
              );
            },
          ),
        );
      },
    );
  }
}
