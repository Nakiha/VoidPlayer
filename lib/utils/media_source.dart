bool isHttpMediaUrl(String value) {
  final uri = Uri.tryParse(value.trim());
  if (uri == null || uri.host.isEmpty) return false;
  final scheme = uri.scheme.toLowerCase();
  return scheme == 'http' || scheme == 'https';
}

String? normalizeNetworkMediaUrl(String value) {
  final trimmed = value.trim();
  return isHttpMediaUrl(trimmed) ? trimmed : null;
}

List<String> mediaSourcesFromDroppedValues(Iterable<String> values) {
  final sources = <String>[];
  final seen = <String>{};

  for (final value in values) {
    for (final source in _sourcesFromDroppedValue(value)) {
      if (seen.add(source)) sources.add(source);
    }
  }

  return sources;
}

Iterable<String> _sourcesFromDroppedValue(String value) sync* {
  final trimmed = value.trim();
  if (trimmed.isEmpty) return;

  final urls = _extractHttpUrls(trimmed);
  if (urls.isNotEmpty) {
    yield* urls;
    return;
  }

  final filePath = _tryFileUriToPath(trimmed);
  yield filePath ?? trimmed;
}

List<String> _extractHttpUrls(String value) {
  final urlPattern = RegExp(r'''https?://[^\s<>"']+''', caseSensitive: false);
  return [
    for (final match in urlPattern.allMatches(value))
      ?normalizeNetworkMediaUrl(_trimUrlBoundary(match.group(0)!)),
  ];
}

String _trimUrlBoundary(String value) {
  return value.replaceFirst(RegExp(r'''[.,;:)\]}]+$'''), '');
}

String? _tryFileUriToPath(String value) {
  final uri = Uri.tryParse(value);
  if (uri == null || uri.scheme.toLowerCase() != 'file') return null;
  try {
    return uri.toFilePath(windows: true);
  } catch (_) {
    return null;
  }
}
