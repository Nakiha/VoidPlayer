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

    for (var i = 0; i < args.length; i++) {
      final arg = args[i];
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
        } else if (arg == '--deep-link' && i + 1 < args.length) {
          final parsed = _parseDeepLink(args[++i]);
          loopRange = parsed.loopRange ?? loopRange;
          warnings.addAll(parsed.warnings);
        } else if (arg.startsWith('--deep-link=')) {
          final parsed = _parseDeepLink(arg.substring('--deep-link='.length));
          loopRange = parsed.loopRange ?? loopRange;
          warnings.addAll(parsed.warnings);
        }
      } catch (e) {
        warnings.add('Ignored invalid startup argument "$arg": $e');
      }
    }

    return StartupOptions(loopRange: loopRange, warnings: warnings);
  }
}

StartupOptions _parseDeepLink(String value) {
  final warnings = <String>[];
  final uri = Uri.tryParse(value);
  if (uri == null || uri.scheme.toLowerCase() != 'voidplayer') {
    return StartupOptions(
      warnings: ['Ignored unsupported deep link: "$value"'],
    );
  }

  final version = uri.host.toLowerCase();
  final action = uri.pathSegments.isNotEmpty ? uri.pathSegments.first : '';
  if (version != 'v1' || action != 'open') {
    return StartupOptions(
      warnings: ['Ignored unsupported deep link route: "$value"'],
    );
  }

  StartupLoopRange? loopRange;
  try {
    final query = uri.queryParameters;
    final rangeValue = query['loopRange'] ?? query['loop-range'];
    if (rangeValue != null) {
      loopRange = _parseRange(rangeValue, defaultUnit: _TimeUnit.seconds);
    } else if (query['loopStart'] != null && query['loopEnd'] != null) {
      final startUs = _parseTimeUs(
        query['loopStart']!,
        defaultUnit: _TimeUnit.seconds,
      );
      final endUs = _parseTimeUs(
        query['loopEnd']!,
        defaultUnit: _TimeUnit.seconds,
      );
      if (startUs < 0 || endUs <= startUs) {
        throw FormatException(
          'expected 0 <= loopStart < loopEnd, got $startUs:$endUs us',
        );
      }
      loopRange = StartupLoopRange(startUs: startUs, endUs: endUs);
    }
  } catch (e) {
    warnings.add('Ignored invalid deep link "$value": $e');
  }

  return StartupOptions(loopRange: loopRange, warnings: warnings);
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
