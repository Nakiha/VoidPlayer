import 'dart:convert';
import 'dart:io';
import 'package:path/path.dart' as p;

/// Manages the on-disk analysis cache in `exe_dir/cache`.
///
/// Cache structure:
/// ```
/// cache/
///   analysis_index.json
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
    return _isVbi2(vbiPath(hash)) && File(vbtPath(hash)).existsSync();
  }

  static bool _isVbi2(String path) {
    final file = File(path);
    if (!file.existsSync()) return false;
    try {
      final raf = file.openSync();
      try {
        final header = raf.readSync(4);
        return header.length == 4 &&
            header[0] == 0x56 &&
            header[1] == 0x42 &&
            header[2] == 0x49 &&
            header[3] == 0x32;
      } finally {
        raf.closeSync();
      }
    } catch (_) {
      return false;
    }
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
      'size': await File(videoPath).length(),
      'mtime': (await File(videoPath).lastModified()).toIso8601String(),
      'time': DateTime.now().toIso8601String(),
    };
    await saveIndex(index);
  }

  static bool hasEntry(String hash, {String? videoPath}) {
    final index = loadIndex();
    final entries = index['entries'] as Map<String, dynamic>;
    if (!entries.containsKey(hash) || !filesExist(hash)) return false;
    if (videoPath == null) return true;

    final entry = entries[hash];
    if (entry is! Map<String, dynamic>) return false;
    final file = File(videoPath);
    if (!file.existsSync()) return false;
    final size = entry['size'];
    final mtime = entry['mtime'];
    if (size is! int || mtime is! String) return false;

    try {
      return file.lengthSync() == size &&
          file.lastModifiedSync().toIso8601String() == mtime;
    } catch (_) {
      return false;
    }
  }
}
