import 'package:flutter/material.dart';
import '../video_renderer_controller.dart';
import 'track_row.dart';

/// Timeline track list area matching PySide6 TimelineArea.
/// Max height: 40% of parent height. Each TrackRow is 40px.
class TimelineArea extends StatelessWidget {
  final List<TrackInfo> tracks;
  final double playheadPosition; // 0.0 - 1.0
  final Map<int, bool> visibility;
  final Map<int, bool> muted;
  final Map<int, int> syncOffsets;
  final ValueChanged<int> onRemoveTrack;
  final void Function(int slot, bool visible) onToggleVisibility;
  final void Function(int slot, bool muted) onToggleMute;
  final void Function(int slot, int offsetMs) onOffsetChanged;

  const TimelineArea({
    super.key,
    required this.tracks,
    this.playheadPosition = 0.0,
    this.visibility = const {},
    this.muted = const {},
    this.syncOffsets = const {},
    required this.onRemoveTrack,
    required this.onToggleVisibility,
    required this.onToggleMute,
    required this.onOffsetChanged,
  });

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, constraints) {
      final maxHeight = constraints.maxHeight;
      final targetHeight = (tracks.length * 40.0).clamp(0.0, maxHeight);
      return SizedBox(
        height: targetHeight,
        child: ListView.builder(
          itemCount: tracks.length,
          itemExtent: 40,
          physics: const NeverScrollableScrollPhysics(),
          itemBuilder: (context, index) {
            final track = tracks[index];
            return TrackRow(
              track: track,
              playheadPosition: playheadPosition,
              onRemove: () => onRemoveTrack(track.slot),
              onToggleVisibility: () =>
                  onToggleVisibility(track.slot, !(visibility[track.slot] ?? true)),
              onToggleMute: () =>
                  onToggleMute(track.slot, !(muted[track.slot] ?? false)),
              onOffsetChanged: (delta) => onOffsetChanged(
                  track.slot, (syncOffsets[track.slot] ?? 0) + delta),
              isVisible: visibility[track.slot] ?? true,
              isMuted: muted[track.slot] ?? false,
              syncOffsetMs: syncOffsets[track.slot] ?? 0,
            );
          },
        ),
      );
    });
  }
}
