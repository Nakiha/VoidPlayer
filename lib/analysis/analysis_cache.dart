import 'dart:convert';
import 'dart:io';
import 'dart:isolate';
import 'package:path/path.dart' as p;

class AnalysisCacheEntryStats {
  final String hash;
  final String name;
  final String? videoPath;
  final int videoBytes;
  final int vbs3Bytes;
  final int vbiBytes;
  final int vbtBytes;
  final DateTime? cachedAt;
  final bool complete;

  const AnalysisCacheEntryStats({
    required this.hash,
    required this.name,
    required this.videoPath,
    required this.videoBytes,
    required this.vbs3Bytes,
    required this.vbiBytes,
    required this.vbtBytes,
    required this.cachedAt,
    required this.complete,
  });

  int get cacheBytes => vbs3Bytes + vbiBytes + vbtBytes;
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
///   <hash>.vbs3
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

  static String vbs3Path(String hash) => p.join(dataDir, '$hash.vbs3');
  static String vbiPath(String hash) => p.join(dataDir, '$hash.vbi');
  static String vbtPath(String hash) => p.join(dataDir, '$hash.vbt');

  static bool filesExist(String hash) {
    final vbi = vbiPath(hash);
    final codec = _vbi2Codec(vbi);
    if (codec == null) return false;
    if (!File(vbtPath(hash)).existsSync()) return false;
    if (codec == _vbiCodecVvc && !File(vbs3Path(hash)).existsSync()) {
      return false;
    }
    return true;
  }

  static const int _vbiCodecVvc = 3;

  static int? _vbi2Codec(String path) {
    final file = File(path);
    if (!file.existsSync()) return null;
    try {
      final raf = file.openSync();
      try {
        final header = raf.readSync(8);
        if (header.length == 8 &&
            header[0] == 0x56 &&
            header[1] == 0x42 &&
            header[2] == 0x49 &&
            header[3] == 0x32) {
          return header[6] | (header[7] << 8);
        }
        return null;
      } finally {
        raf.closeSync();
      }
    } catch (_) {
      return null;
    }
  }

  static File get indexFile => File(p.join(dataDir, 'analysis_index.json'));

  static Future<AnalysisCacheSnapshot> snapshot({int maxBytes = 0}) async {
    return Isolate.run(() => scan(maxBytes: maxBytes));
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

        final vbs3 = _fileLength(vbs3Path(hash));
        final vbi = _fileLength(vbiPath(hash));
        final vbt = _fileLength(vbtPath(hash));
        final cacheBytes = vbs3 + vbi + vbt;
        indexedBytes += cacheBytes;

        entries.add(
          AnalysisCacheEntryStats(
            hash: hash,
            name: value['name'] as String? ?? hash,
            videoPath: value['path'] as String?,
            videoBytes: (value['size'] as num?)?.toInt() ?? 0,
            vbs3Bytes: vbs3,
            vbiBytes: vbi,
            vbtBytes: vbt,
            cachedAt: DateTime.tryParse(value['time'] as String? ?? ''),
            complete: filesExist(hash),
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
    final fallback = {'entries': <String, dynamic>{}};
    final f = indexFile;
    if (!f.existsSync()) return fallback;
    try {
      final raw = f.readAsStringSync();
      final decoded = jsonDecode(raw);
      if (decoded is! Map) return fallback;
      final index = Map<String, dynamic>.from(decoded);
      final entries = index['entries'];
      index['entries'] = entries is Map
          ? Map<String, dynamic>.from(entries)
          : <String, dynamic>{};
      return index;
    } catch (_) {
      return fallback;
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
    final entries = _entriesFromIndex(index);
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
      for (final path in [vbs3Path(hash), vbiPath(hash), vbtPath(hash)]) {
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
    final entries = _entriesFromIndex(index);
    if (!entries.containsKey(hash) || !filesExist(hash)) return false;
    if (videoPath == null) return true;
    return File(videoPath).existsSync();
  }

  static Map<String, dynamic> _entriesFromIndex(Map<String, dynamic> index) {
    final rawEntries = index['entries'];
    if (rawEntries is Map<String, dynamic>) return rawEntries;
    if (rawEntries is Map) {
      final entries = Map<String, dynamic>.from(rawEntries);
      index['entries'] = entries;
      return entries;
    }
    final entries = <String, dynamic>{};
    index['entries'] = entries;
    return entries;
  }
}
