import 'dart:io';

({double width, double height})? parseTestWindowHeader(String scriptPath) {
  try {
    final file = File(scriptPath);
    if (!file.existsSync()) return null;
    for (final rawLine in file.readAsLinesSync()) {
      final line = rawLine.trim();
      if (!line.startsWith('@')) continue;
      final parts = line.split(',').map((s) => s.trim()).toList();
      if (parts.isEmpty) continue;
      final key = parts.first.toUpperCase();
      if (key == '@WINDOW' && parts.length >= 3) {
        return (width: double.parse(parts[1]), height: double.parse(parts[2]));
      }
    }
  } on FormatException {
    return null;
  } on FileSystemException {
    return null;
  }
  return null;
}

bool hasCliFlag(List<String> args, String name) =>
    args.any((arg) => arg == name || arg.startsWith('$name='));
