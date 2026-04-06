import 'package:flutter/material.dart';

/// Time label displaying current / total in MM:SS.mmm format.
/// Matches PySide6 TimeLabel widget.
class TimeLabel extends StatelessWidget {
  final int currentUs;
  final int totalUs;

  const TimeLabel({
    super.key,
    required this.currentUs,
    required this.totalUs,
  });

  static String formatUs(int us) {
    if (us < 0) us = 0;
    final totalMs = us ~/ 1000;
    final minutes = totalMs ~/ 60000;
    final seconds = (totalMs % 60000) ~/ 1000;
    final millis = totalMs % 1000;
    return '${minutes.toString().padLeft(2, '0')}:'
        '${seconds.toString().padLeft(2, '0')}.'
        '${millis.toString().padLeft(3, '0')}';
  }

  @override
  Widget build(BuildContext context) {
    return Text(
      '${formatUs(currentUs)} / ${formatUs(totalUs)}',
      style: Theme.of(context).textTheme.bodySmall?.copyWith(
            fontFeatures: [const FontFeature.tabularFigures()],
          ),
    );
  }
}
