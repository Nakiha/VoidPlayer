/// Format microseconds to `M:SS.ss` (zero-padded seconds, variable-width minutes).
String formatTimeShort(int us) {
  final totalSeconds = us / 1000000.0;
  final minutes = totalSeconds.toInt() ~/ 60;
  final seconds = totalSeconds - minutes * 60;
  return '$minutes:${seconds.toStringAsFixed(2).padLeft(5, '0')}';
}

/// Format microseconds to `MM:SS.ss` (zero-padded minutes and seconds).
String formatTimePad2(int us) {
  final totalSeconds = us / 1000000.0;
  final minutes = totalSeconds.toInt() ~/ 60;
  final seconds = totalSeconds - minutes * 60;
  return '${minutes.toString().padLeft(2, '0')}:'
      '${seconds.toStringAsFixed(2).padLeft(5, '0')}';
}
