import 'dart:convert';
import 'dart:io';
import 'package:path/path.dart' as p;

class AnalysisCacheEntryStats {
  final String hash;
  final String name;
  final String? videoPath;
  final int videoBytes;
  final int vbs2Bytes;
  final int vbiBytes;
  final int vbtBytes;
  final DateTime? cachedAt;
  final bool complete;

  const AnalysisCacheEntryStats({
    required this.hash,
    required this.name,
    required this.videoPath,
    required this.videoBytes,
    required this.vbs2Bytes,
    required this.vbiBytes,
    required this.vbtBytes,
    required this.cachedAt,
    required this.complete,
  });

  int get cacheBytes => vbs2Bytes + vbiBytes + vbtBytes;
}

class AnalysisCacheSnapshot {
  final String path;
  final int totalBytes;
  final int indexedBytes;
  final int unindexedBytes;
  final int maxBytes;
  final List<AnalysisCacheEntryStats> entries;

  const AnalysisCacheSnapshot({
    required this.path,
    required this.totalBytes,
    required this.indexedBytes,
    required this.unindexedBytes,
    required this.maxBytes,
    required this.entries,
  });

  bool get hasLimit => maxBytes > 0;
  bool get isOverLimit => hasLimit && totalBytes >= maxBytes;
  int get remainingBytes =>
      hasLimit ? (maxBytes - totalBytes).clamp(0, maxBytes) : 0;
  double get usageFraction {
    if (!hasLimit) return 0;
    return (totalBytes / maxBytes).clamp(0.0, 1.0);
  }
}

class AnalysisCacheDeleteResult {
  final List<String> deletedHashes;
  final Map<String, List<String>> failuresByHash;

  const AnalysisCacheDeleteResult({
    required this.deletedHashes,
    required this.failuresByHash,
  });

  int get deletedCount => deletedHashes.length;
  int get failedCount => failuresByHash.length;
  bool get hasFailures => failuresByHash.isNotEmpty;
}

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

  static Future<AnalysisCacheSnapshot> snapshot({int maxBytes = 0}) async {
    return scan(maxBytes: maxBytes);
  }

  static AnalysisCacheSnapshot scan({int maxBytes = 0}) {
    final dir = Directory(dataDir);
    if (!dir.existsSync()) {
      return AnalysisCacheSnapshot(
        path: dataDir,
        totalBytes: 0,
        indexedBytes: 0,
        unindexedBytes: 0,
        maxBytes: maxBytes,
        entries: const [],
      );
    }

    var totalBytes = 0;
    for (final entity in dir.listSync(recursive: false, followLinks: false)) {
      if (entity is File) {
        try {
          totalBytes += entity.lengthSync();
        } catch (_) {
          // Best-effort stats; skip files that disappear during scanning.
        }
      }
    }

    final index = loadIndex();
    final rawEntries = index['entries'];
    final entries = <AnalysisCacheEntryStats>[];
    var indexedBytes = 0;

    if (rawEntries is Map<String, dynamic>) {
      for (final item in rawEntries.entries) {
        final hash = item.key;
        final value = item.value;
        if (value is! Map<String, dynamic>) continue;

        final vbs2 = _fileLength(vbs2Path(hash));
        final vbi = _fileLength(vbiPath(hash));
        final vbt = _fileLength(vbtPath(hash));
        final cacheBytes = vbs2 + vbi + vbt;
        indexedBytes += cacheBytes;

        entries.add(
          AnalysisCacheEntryStats(
            hash: hash,
            name: value['name'] as String? ?? hash,
            videoPath: value['path'] as String?,
            videoBytes: (value['size'] as num?)?.toInt() ?? 0,
            vbs2Bytes: vbs2,
            vbiBytes: vbi,
            vbtBytes: vbt,
            cachedAt: DateTime.tryParse(value['time'] as String? ?? ''),
            complete:
                _isVbi2(vbiPath(hash)) && File(vbtPath(hash)).existsSync(),
          ),
        );
      }
    }

    entries.sort((a, b) => b.cacheBytes.compareTo(a.cacheBytes));
    return AnalysisCacheSnapshot(
      path: dataDir,
      totalBytes: totalBytes,
      indexedBytes: indexedBytes,
      unindexedBytes: (totalBytes - indexedBytes).clamp(0, totalBytes),
      maxBytes: maxBytes,
      entries: entries,
    );
  }

  static int _fileLength(String path) {
    try {
      final file = File(path);
      return file.existsSync() ? file.lengthSync() : 0;
    } catch (_) {
      return 0;
    }
  }

  static String formatBytes(int bytes) {
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    var value = bytes.toDouble();
    var unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit++;
    }
    if (unit == 0) return '$bytes B';
    return '${value.toStringAsFixed(value >= 10 ? 1 : 2)} ${units[unit]}';
  }

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

  static Future<AnalysisCacheDeleteResult> deleteEntries(
    Iterable<String> hashes,
  ) async {
    final uniqueHashes = hashes.toSet();
    final index = loadIndex();
    final rawEntries = index['entries'];
    final entries = rawEntries is Map<String, dynamic>
        ? rawEntries
        : <String, dynamic>{};

    final deletedHashes = <String>[];
    final failuresByHash = <String, List<String>>{};

    for (final hash in uniqueHashes) {
      final failures = <String>[];
      for (final path in [vbs2Path(hash), vbiPath(hash), vbtPath(hash)]) {
        final file = File(path);
        try {
          if (file.existsSync()) await file.delete();
        } on FileSystemException catch (e) {
          failures.add(e.path ?? path);
        } catch (_) {
          failures.add(path);
        }
      }

      if (failures.isEmpty) {
        entries.remove(hash);
        deletedHashes.add(hash);
      } else {
        failuresByHash[hash] = failures;
      }
    }

    if (deletedHashes.isNotEmpty) {
      index['entries'] = entries;
      await saveIndex(index);
    }

    return AnalysisCacheDeleteResult(
      deletedHashes: deletedHashes,
      failuresByHash: failuresByHash,
    );
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
