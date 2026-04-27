class StartupLoopRange {
  final int startUs;
  final int endUs;

  const StartupLoopRange({required this.startUs, required this.endUs});
}

class StartupOptions {
  final StartupLoopRange? loopRange;
  final List<String> warnings;

  const StartupOptions({this.loopRange, this.warnings = const []});

  static StartupOptions parse(List<String> args) {
    StartupLoopRange? loopRange;
    final warnings = <String>[];

    for (final arg in args) {
      try {
        if (arg.startsWith('--loop-range-us=')) {
          loopRange = _parseRange(
            arg.substring('--loop-range-us='.length),
            defaultUnit: _TimeUnit.microseconds,
          );
        } else if (arg.startsWith('--loop-range=')) {
          loopRange = _parseRange(
            arg.substring('--loop-range='.length),
            defaultUnit: _TimeUnit.seconds,
          );
        }
      } catch (e) {
        warnings.add('Ignored invalid startup argument "$arg": $e');
      }
    }

    return StartupOptions(loopRange: loopRange, warnings: warnings);
  }
}

enum _TimeUnit { seconds, milliseconds, microseconds }

StartupLoopRange _parseRange(String value, {required _TimeUnit defaultUnit}) {
  final parts = _splitRange(value);
  if (parts == null) {
    throw const FormatException(
      'expected "start:end", "start..end", or "start,end"',
    );
  }

  final startUs = _parseTimeUs(parts.$1, defaultUnit: defaultUnit);
  final endUs = _parseTimeUs(parts.$2, defaultUnit: defaultUnit);
  if (startUs < 0 || endUs <= startUs) {
    throw FormatException('expected 0 <= start < end, got $startUs:$endUs us');
  }
  return StartupLoopRange(startUs: startUs, endUs: endUs);
}

(String, String)? _splitRange(String value) {
  for (final separator in const [':', '..', ',']) {
    final index = value.indexOf(separator);
    if (index <= 0) continue;
    final endIndex = index + separator.length;
    if (endIndex >= value.length) continue;
    return (value.substring(0, index).trim(), value.substring(endIndex).trim());
  }
  return null;
}

int _parseTimeUs(String value, {required _TimeUnit defaultUnit}) {
  final normalized = value.trim().toLowerCase();
  if (normalized.isEmpty) {
    throw const FormatException('empty time value');
  }

  if (normalized.endsWith('us')) {
    return double.parse(normalized.substring(0, normalized.length - 2)).round();
  }
  if (normalized.endsWith('ms')) {
    return (double.parse(normalized.substring(0, normalized.length - 2)) * 1000)
        .round();
  }
  if (normalized.endsWith('s')) {
    return (double.parse(normalized.substring(0, normalized.length - 1)) *
            1000000)
        .round();
  }

  final raw = double.parse(normalized);
  return switch (defaultUnit) {
    _TimeUnit.seconds => (raw * 1000000).round(),
    _TimeUnit.milliseconds => (raw * 1000).round(),
    _TimeUnit.microseconds => raw.round(),
  };
}
