import 'package:flutter/material.dart';
import 'zoom_combo_box.dart';
import 'time_label.dart';
import 'timeline_slider.dart';

/// Bottom playback controls bar matching PySide6 ControlsBar (40px height).
class ControlsBar extends StatelessWidget {
  final double zoomRatio;
  final ValueChanged<double> onZoomChanged;
  final bool isPlaying;
  final VoidCallback onTogglePlay;
  final VoidCallback onStepForward;
  final VoidCallback onStepBackward;
  final int currentPtsUs;
  final int durationUs;
  final ValueChanged<int> onSeek;

  const ControlsBar({
    super.key,
    required this.zoomRatio,
    required this.onZoomChanged,
    required this.isPlaying,
    required this.onTogglePlay,
    required this.onStepForward,
    required this.onStepBackward,
    required this.currentPtsUs,
    required this.durationUs,
    required this.onSeek,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      height: 40,
      padding: const EdgeInsets.all(4),
      child: Row(
        children: [
          // Zoom combo
          ZoomComboBox(
            value: zoomRatio,
            onChanged: onZoomChanged,
          ),
          const SizedBox(width: 4),
          // Step backward
          SizedBox(
            width: 32,
            height: 32,
            child: IconButton(
              onPressed: onStepBackward,
              icon: const Icon(Icons.skip_previous, size: 18),
              tooltip: 'Previous Frame',
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints.tightFor(width: 32, height: 32),
            ),
          ),
          // Play/Pause
          SizedBox(
            width: 32,
            height: 32,
            child: IconButton(
              onPressed: onTogglePlay,
              icon: Icon(
                isPlaying ? Icons.pause : Icons.play_arrow,
                size: 20,
              ),
              tooltip: isPlaying ? 'Pause' : 'Play',
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints.tightFor(width: 32, height: 32),
            ),
          ),
          // Step forward
          SizedBox(
            width: 32,
            height: 32,
            child: IconButton(
              onPressed: onStepForward,
              icon: const Icon(Icons.skip_next, size: 18),
              tooltip: 'Next Frame',
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints.tightFor(width: 32, height: 32),
            ),
          ),
          const SizedBox(width: 4),
          // Time label
          TimeLabel(currentUs: currentPtsUs, totalUs: durationUs),
          const SizedBox(width: 4),
          // Timeline slider (expanded)
          Expanded(
            child: TimelineSlider(
              currentUs: currentPtsUs,
              durationUs: durationUs,
              onSeek: onSeek,
            ),
          ),
        ],
      ),
    );
  }
}
