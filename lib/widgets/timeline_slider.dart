import 'package:flutter/material.dart';

/// Seek slider matching PySide6 TimelineSlider. Maps 0..duration to seek position.
class TimelineSlider extends StatelessWidget {
  final int currentUs;
  final int durationUs;
  final ValueChanged<int> onSeek;

  const TimelineSlider({
    super.key,
    required this.currentUs,
    required this.durationUs,
    required this.onSeek,
  });

  @override
  Widget build(BuildContext context) {
    final double value = durationUs > 0 ? currentUs / durationUs : 0.0;
    return SliderTheme(
      data: SliderThemeData(
        trackHeight: 4,
        thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 6),
        overlayShape: const RoundSliderOverlayShape(overlayRadius: 12),
        activeTrackColor: Theme.of(context).colorScheme.primary,
        inactiveTrackColor: Theme.of(context).colorScheme.surfaceContainerHighest,
      ),
      child: Slider(
        value: value.clamp(0.0, 1.0),
        onChanged: (v) {
          if (durationUs > 0) {
            onSeek((v * durationUs).round());
          }
        },
        onChangeEnd: (v) {
          if (durationUs > 0) {
            onSeek((v * durationUs).round());
          }
        },
      ),
    );
  }
}
