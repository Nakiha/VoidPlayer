import 'package:flutter/material.dart';
import '../track_manager.dart';
import '../utils/pts_range.dart';
import 'track_row.dart';

/// Timeline track list area matching PySide6 TimelineArea.
const double timelineTrackRowHeight = 32.0;

/// Max height: 40% of parent height. Each TrackRow is 32px.
/// Supports drag-to-reorder via [ReorderableListView].
/// Dragging any row's divider resizes all rows synchronously.
class TimelineArea extends StatefulWidget {
  final List<TrackEntry> entries;
  final int currentPtsUs;
  final void Function(int oldIndex, int newIndex) onReorder;
  final void Function(int slot, int offsetMs) onOffsetChanged;
  final ValueChanged<int> onToggleTrackAudio;
  final int? audibleTrackFileId;
  final ValueChanged<int> onRemoveTrack;
  final bool canRemoveTrack;
  final bool canReorderTrack;
  final bool canAdjustTrackOffset;
  final bool canToggleTrackAudio;
  final Map<int, int> syncOffsets; // fileId -> offset in microseconds
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
    required this.onToggleTrackAudio,
    this.audibleTrackFileId,
    required this.onRemoveTrack,
    this.canRemoveTrack = true,
    this.canReorderTrack = true,
    this.canAdjustTrackOffset = true,
    this.canToggleTrackAudio = true,
    this.syncOffsets = const {},
    this.maxEffectiveDurationUs = 0,
    this.hoverPtsUs = 0,
    this.sliderHovering = false,
    this.controlsWidth = 332,
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
        final targetHeight = (widget.entries.length * timelineTrackRowHeight)
            .clamp(0.0, maxHeight);
        return SizedBox(
          height: targetHeight,
          child: ReorderableListView.builder(
            buildDefaultDragHandles: false,
            padding: EdgeInsets.zero,
            itemCount: widget.entries.length,
            onReorder: widget.canReorderTrack ? widget.onReorder : (_, _) {},
            itemBuilder: (context, index) {
              final entry = widget.entries[index];
              final trackDuration = entry.info.durationUs;
              final playableDuration = trackPlayableDurationUs(
                startTimeUs: entry.info.startTimeUs,
                durationUs: entry.info.durationUs,
              );
              final trackStartTimeUs = entry.info.startTimeUs;
              final offsetUs = widget.syncOffsets[entry.fileId] ?? 0;
              final globalTrackStartUs = trackStartTimeUs + offsetUs;

              // Clip ratio: original duration relative to max effective duration
              final clipRatio = maxEffectiveDurationUs > 0
                  ? (playableDuration / maxEffectiveDurationUs).clamp(0.0, 1.0)
                  : 1.0;

              // Offset ratio: where the clip block starts
              final offsetRatio = maxEffectiveDurationUs > 0
                  ? (globalTrackStartUs / maxEffectiveDurationUs).clamp(
                      0.0,
                      1.0,
                    )
                  : 0.0;

              // Per-track playhead: global time → track internal time
              double playheadPosition = 0.0;
              if (playableDuration > 0) {
                final localTime = widget.currentPtsUs - globalTrackStartUs;
                playheadPosition = (localTime / playableDuration).clamp(
                  0.0,
                  1.0,
                );
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
                    widget.onOffsetChanged(entry.fileId, delta),
                onToggleAudio: () => widget.onToggleTrackAudio(entry.fileId),
                canRemove: widget.canRemoveTrack,
                canReorder: widget.canReorderTrack,
                canAdjustOffset: widget.canAdjustTrackOffset,
                canToggleAudio: widget.canToggleTrackAudio,
                isAudible: widget.audibleTrackFileId == entry.fileId,
                syncOffsetMs: offsetUs ~/ 1000,
                controlsWidth: widget.controlsWidth,
                onControlsWidthChanged: widget.onControlsWidthChanged,
                hoverPtsUs: widget.hoverPtsUs,
                sliderHovering: widget.sliderHovering,
                trackDurationUs: trackDuration,
                trackStartTimeUs: trackStartTimeUs,
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
