import 'dart:convert';
import 'dart:io';
import 'package:path/path.dart' as p;

/// Manages the on-disk analysis cache in `exe_dir/cache`.
///
/// Cache structure:
/// ```
/// cache/
///   analysis_index.json   ← { "entries": { "<hash>": { "name": "...", "path": "...", "time": "..." } } }
///   <hash>.vbs2
///   <hash>.vbi
///   <hash>.vbt
/// ```
class AnalysisCache {
  AnalysisCache._();

  static final String dataDir = _resolveDataDir();

  static String _resolveDataDir() {
    final exeDir = p.dirname(Platform.resolvedExecutable);
    return p.join(exeDir, 'cache');
  }

  // ---- Path helpers ----

  static String vbs2Path(String hash) => p.join(dataDir, '$hash.vbs2');
  static String vbiPath(String hash) => p.join(dataDir, '$hash.vbi');
  static String vbtPath(String hash) => p.join(dataDir, '$hash.vbt');

  static bool filesExist(String hash) {
    // VBS2 is optional (requires VTM decoder)
    return File(vbiPath(hash)).existsSync() && File(vbtPath(hash)).existsSync();
  }

  static File get indexFile => File(p.join(dataDir, 'analysis_index.json'));

  // ---- Index operations ----

  static Map<String, dynamic> loadIndex() {
    final f = indexFile;
    if (!f.existsSync()) return {'entries': <String, dynamic>{}};
    try {
      final raw = f.readAsStringSync();
      return jsonDecode(raw) as Map<String, dynamic>;
    } catch (_) {
      return {'entries': <String, dynamic>{}};
    }
  }

  static Future<void> saveIndex(Map<String, dynamic> index) async {
    await Directory(dataDir).create(recursive: true);
    await indexFile.writeAsString(
      const JsonEncoder.withIndent('  ').convert(index),
    );
  }

  static Future<void> addEntry(
    String hash,
    String name,
    String videoPath,
  ) async {
    final index = loadIndex();
    final entries = index['entries'] as Map<String, dynamic>;
    entries[hash] = {
      'name': name,
      'path': videoPath,
      'time': DateTime.now().toIso8601String(),
    };
    await saveIndex(index);
  }

  static bool hasEntry(String hash) {
    final index = loadIndex();
    final entries = index['entries'] as Map<String, dynamic>;
    return entries.containsKey(hash) && filesExist(hash);
  }
}
