import 'dart:convert';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';
import 'package:path/path.dart' as p;

import '../app_paths.dart';
import '../utils/atomic_file.dart';
import '../utils/file_lock.dart';

class AnalysisCacheEntryStats {
  final String hash;
  final String name;
  final String? videoPath;
  final int videoBytes;
  final int analysisBytes;
  final DateTime? cachedAt;
  final DateTime? lastAccessedAt;
  final bool complete;

  const AnalysisCacheEntryStats({
    required this.hash,
    required this.name,
    required this.videoPath,
    required this.videoBytes,
    required this.analysisBytes,
    required this.cachedAt,
    required this.lastAccessedAt,
    required this.complete,
  });

  int get cacheBytes => analysisBytes;
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

class AnalysisCachePruneResult {
  final AnalysisCacheSnapshot snapshot;
  final AnalysisCacheDeleteResult deleteResult;

  const AnalysisCachePruneResult({
    required this.snapshot,
    required this.deleteResult,
  });

  bool get withinLimit => !snapshot.isOverLimit;
  int get deletedCount => deleteResult.deletedCount;
  bool get hasFailures => deleteResult.hasFailures;
}

/// Manages the on-disk analysis cache in the resolved app data root.
///
/// Cache structure:
/// ```
/// cache/
///   analysis_index.json
///   <hash>/
///     base.vac
///     chunks/
///     tmp/
///   <hash>.vac  # legacy VAC1 artifact, ignored as a read path
/// ```
class AnalysisCache {
  AnalysisCache._();

  static const int currentVacVersion = 1;
  static const int currentVac2MajorVersion = 2;
  static const int _vacHeaderSize = 64;
  static const int _vacSectionEntrySize = 48;
  static const int _vac2HeaderSize = 124;
  static const int _vac2SectionEntrySize = 56;

  static final String dataDir = AppPaths.current.analysisCacheDir;
  static String get _locksDir => p.join(dataDir, 'locks');
  static String get _indexLockPath => p.join(_locksDir, 'analysis_index.lock');

  static String hashLockPath(String hash) => p.join(_locksDir, '$hash.lock');

  static Future<T> withHashExclusiveLock<T>(
    String hash,
    Future<T> Function() action,
  ) {
    return FileLockService.withExclusive(hashLockPath(hash), action);
  }

  static FileLockHandle acquireHashSharedLockSync(String hash) {
    return FileLockService.acquireSharedSync(hashLockPath(hash));
  }

  // ---- Path helpers ----

  static String hashDir(String hash) => p.join(dataDir, hash);

  static String vac2BasePath(String hash) => p.join(hashDir(hash), 'base.vac');

  static String chunksDir(String hash) => p.join(hashDir(hash), 'chunks');

  static String overlayChunksDir(String hash) =>
      p.join(chunksDir(hash), 'overlay');

  static String legacyAnalysisPath(String hash) => p.join(dataDir, '$hash.vac');

  static String analysisPath(String hash) => vac2BasePath(hash);

  static String hashForAnalysisPath(String analysisPath) {
    if (p.basename(analysisPath).toLowerCase() == 'base.vac') {
      return p.basename(p.dirname(analysisPath));
    }
    return p.basenameWithoutExtension(analysisPath);
  }

  static bool filesExist(String hash) => _isCompleteVac2(vac2BasePath(hash));

  static bool hasLegacyAnalysis(String hash) => false;

  static bool hasOverlayChunks(String hash) {
    final dir = Directory(overlayChunksDir(hash));
    if (!dir.existsSync()) return false;
    try {
      return dir.listSync(followLinks: false).any((entity) {
        if (entity is! File) return false;
        return p.extension(entity.path).toLowerCase() == '.vck';
      });
    } catch (_) {
      return false;
    }
  }

  static bool deleteIfVacVersionMismatch(String hash) {
    final basePath = vac2BasePath(hash);
    if (File(basePath).existsSync() && !_isCompleteVac2(basePath)) {
      final result = _deleteEntriesSync([hash]);
      return result.deletedHashes.contains(hash);
    }
    return false;
  }

  static int? readVacVersion(String path) {
    final file = File(path);
    if (!file.existsSync()) return null;
    try {
      final raf = file.openSync();
      try {
        if (raf.lengthSync() < 6) return null;
        final bytes = raf.readSync(6);
        if (bytes.length != 6 ||
            bytes[0] != 0x56 ||
            bytes[1] != 0x41 ||
            bytes[2] != 0x43 ||
            bytes[3] != 0x31) {
          return null;
        }
        final data = ByteData.sublistView(Uint8List.fromList(bytes));
        return data.getUint16(4, Endian.little);
      } finally {
        raf.closeSync();
      }
    } catch (_) {
      return null;
    }
  }

  static bool hasIncompleteContainer(String hash) {
    final basePath = vac2BasePath(hash);
    if (File(basePath).existsSync() && !_isCompleteVac2(basePath)) return true;

    final legacyPath = legacyAnalysisPath(hash);
    if (File(legacyPath).existsSync() && !_isCompleteVac1(legacyPath)) {
      return true;
    }
    return false;
  }

  static bool _isCompleteVac1(String path) {
    final file = File(path);
    if (!file.existsSync()) return false;
    try {
      final raf = file.openSync();
      try {
        final length = raf.lengthSync();
        if (length < _vacHeaderSize) return false;
        final header = raf.readSync(4);
        if (header.length != 4 ||
            header[0] != 0x56 ||
            header[1] != 0x41 ||
            header[2] != 0x43 ||
            header[3] != 0x31) {
          return false;
        }
        raf.setPositionSync(0);
        final bytes = raf.readSync(_vacHeaderSize);
        if (bytes.length < _vacHeaderSize) return false;
        final data = ByteData.sublistView(Uint8List.fromList(bytes));
        final version = data.getUint16(4, Endian.little);
        final headerSize = data.getUint16(6, Endian.little);
        final sectionEntrySize = data.getUint16(8, Endian.little);
        final sectionCount = data.getUint16(10, Endian.little);
        final sectionTableOffset = data.getUint64(16, Endian.little);
        final expectedFileSize = data.getUint64(24, Endian.little);
        if (version != currentVacVersion ||
            headerSize != _vacHeaderSize ||
            sectionEntrySize != _vacSectionEntrySize ||
            sectionCount == 0 ||
            sectionCount > 16 ||
            expectedFileSize != length) {
          return false;
        }
        final tableBytes = sectionCount * sectionEntrySize;
        if (!_rangeFits(sectionTableOffset, tableBytes, length)) return false;
        raf.setPositionSync(sectionTableOffset);
        final table = raf.readSync(tableBytes);
        if (table.length != tableBytes) return false;
        final tableData = ByteData.sublistView(Uint8List.fromList(table));
        for (var i = 0; i < sectionCount; i++) {
          final offset = i * sectionEntrySize;
          final payloadOffset = tableData.getUint64(offset + 8, Endian.little);
          final payloadSize = tableData.getUint64(offset + 16, Endian.little);
          if (payloadSize == 0 ||
              !_rangeFits(payloadOffset, payloadSize, length)) {
            return false;
          }
        }
        return true;
      } finally {
        raf.closeSync();
      }
    } catch (_) {
      return false;
    }
  }

  static bool _isCompleteVac2(String path) {
    final file = File(path);
    if (!file.existsSync()) return false;
    try {
      final raf = file.openSync();
      try {
        final length = raf.lengthSync();
        if (length < _vac2HeaderSize) return false;
        final bytes = raf.readSync(_vac2HeaderSize);
        if (bytes.length < _vac2HeaderSize ||
            bytes[0] != 0x56 ||
            bytes[1] != 0x41 ||
            bytes[2] != 0x43 ||
            bytes[3] != 0x32) {
          return false;
        }
        final data = ByteData.sublistView(Uint8List.fromList(bytes));
        final major = data.getUint16(4, Endian.little);
        final headerSize = data.getUint16(8, Endian.little);
        final sectionEntrySize = data.getUint16(10, Endian.little);
        final sectionCount = data.getUint32(12, Endian.little);
        final sectionTableOffset = data.getUint64(52, Endian.little);
        final expectedFileSize = data.getUint64(60, Endian.little);
        if (major != currentVac2MajorVersion ||
            headerSize != _vac2HeaderSize ||
            sectionEntrySize != _vac2SectionEntrySize ||
            sectionCount == 0 ||
            sectionCount > 64 ||
            expectedFileSize != length) {
          return false;
        }
        final tableBytes = sectionCount * sectionEntrySize;
        if (!_rangeFits(sectionTableOffset, tableBytes, length)) return false;
        raf.setPositionSync(sectionTableOffset);
        final table = raf.readSync(tableBytes);
        if (table.length != tableBytes) return false;
        final tableData = ByteData.sublistView(Uint8List.fromList(table));
        for (var i = 0; i < sectionCount; i++) {
          final offset = i * sectionEntrySize;
          final payloadOffset = tableData.getUint64(offset + 8, Endian.little);
          final payloadSize = tableData.getUint64(offset + 16, Endian.little);
          if (payloadSize == 0 ||
              !_rangeFits(payloadOffset, payloadSize, length)) {
            return false;
          }
        }
        return true;
      } finally {
        raf.closeSync();
      }
    } catch (_) {
      return false;
    }
  }

  static bool _rangeFits(int offset, int size, int fileSize) {
    return offset <= fileSize && size <= fileSize - offset;
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
    for (final entity in dir.listSync(recursive: true, followLinks: false)) {
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

        final cacheBytes = _currentBytesForHashSync(hash);
        indexedBytes += cacheBytes;

        entries.add(
          AnalysisCacheEntryStats(
            hash: hash,
            name: value['name'] as String? ?? hash,
            videoPath: value['path'] as String?,
            videoBytes: (value['size'] as num?)?.toInt() ?? 0,
            analysisBytes: cacheBytes,
            cachedAt: DateTime.tryParse(value['time'] as String? ?? ''),
            lastAccessedAt: DateTime.tryParse(
              value['lastAccessed'] as String? ?? '',
            ),
            complete: filesExist(hash),
          ),
        );
      }
    }

    entries.sort((a, b) {
      final aTime = a.lastAccessedAt ?? a.cachedAt ?? DateTime(0);
      final bTime = b.lastAccessedAt ?? b.cachedAt ?? DateTime(0);
      return bTime.compareTo(aTime);
    });
    return AnalysisCacheSnapshot(
      path: dataDir,
      totalBytes: totalBytes,
      indexedBytes: indexedBytes,
      unindexedBytes: (totalBytes - indexedBytes).clamp(0, totalBytes),
      maxBytes: maxBytes,
      entries: entries,
    );
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
    return FileLockService.withSharedSync(_indexLockPath, _loadIndexUnlocked);
  }

  static Map<String, dynamic> _loadIndexUnlocked() {
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
    await FileLockService.withExclusive(_indexLockPath, () async {
      await _saveIndexUnlocked(index);
    });
  }

  static Future<void> _saveIndexUnlocked(Map<String, dynamic> index) async {
    await AtomicFileWriter.writeString(
      indexFile,
      const JsonEncoder.withIndent('  ').convert(index),
    );
  }

  static void _saveIndexUnlockedSync(Map<String, dynamic> index) {
    AtomicFileWriter.writeStringSync(
      indexFile,
      const JsonEncoder.withIndent('  ').convert(index),
    );
  }

  static Future<void> addEntry(
    String hash,
    String name,
    String videoPath,
  ) async {
    await FileLockService.withExclusive(_indexLockPath, () async {
      final index = _loadIndexUnlocked();
      final entries = _entriesFromIndex(index);
      final existing = entries[hash] is Map
          ? Map<String, dynamic>.from(entries[hash] as Map)
          : <String, dynamic>{};
      final now = DateTime.now().toIso8601String();
      entries[hash] = {
        'name': name,
        'path': videoPath,
        'size': await File(videoPath).length(),
        'mtime': (await File(videoPath).lastModified()).toIso8601String(),
        'time': existing['time'] as String? ?? now,
        'lastAccessed': now,
      };
      await _saveIndexUnlocked(index);
    });
  }

  static Future<void> touchEntry(String hash) async {
    await FileLockService.withExclusive(_indexLockPath, () async {
      final index = _loadIndexUnlocked();
      final entries = _entriesFromIndex(index);
      final rawEntry = entries[hash];
      if (rawEntry is! Map) return;
      final entry = Map<String, dynamic>.from(rawEntry);
      entry['lastAccessed'] = DateTime.now().toIso8601String();
      entries[hash] = entry;
      await _saveIndexUnlocked(index);
    });
  }

  static Future<AnalysisCacheDeleteResult> deleteEntries(
    Iterable<String> hashes,
  ) async {
    return FileLockService.withExclusive(_indexLockPath, () async {
      final uniqueHashes = hashes.toSet();
      final index = _loadIndexUnlocked();
      final rawEntries = index['entries'];
      final entries = rawEntries is Map<String, dynamic>
          ? rawEntries
          : <String, dynamic>{};

      final deletedHashes = <String>[];
      final failuresByHash = <String, List<String>>{};

      for (final hash in uniqueHashes) {
        final failures = await FileLockService.tryExclusive(
          hashLockPath(hash),
          () async => _deleteHashFiles(hash),
        );
        if (failures == null) {
          failuresByHash[hash] = [analysisPath(hash)];
          continue;
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
        await _saveIndexUnlocked(index);
      }

      return AnalysisCacheDeleteResult(
        deletedHashes: deletedHashes,
        failuresByHash: failuresByHash,
      );
    });
  }

  static Future<List<String>> _deleteHashFiles(String hash) async {
    final failures = <String>[];
    for (final path in _pathsForHash(hash)) {
      try {
        final type = await FileSystemEntity.type(path);
        if (type == FileSystemEntityType.directory) {
          await Directory(path).delete(recursive: true);
        } else if (type == FileSystemEntityType.file) {
          await File(path).delete();
        }
      } on FileSystemException catch (e) {
        failures.add(e.path ?? path);
      } catch (_) {
        failures.add(path);
      }
    }
    await _deleteHashStagingDirs(hash, failures);
    return failures;
  }

  static Future<void> _deleteHashStagingDirs(
    String hash,
    List<String> failures,
  ) async {
    final tmpDir = Directory(p.join(dataDir, 'tmp'));
    if (!await tmpDir.exists()) return;
    await for (final entity in tmpDir.list(followLinks: false)) {
      if (entity is! Directory) continue;
      final name = p.basename(entity.path);
      if (!name.startsWith('$hash.')) continue;
      try {
        await entity.delete(recursive: true);
      } on FileSystemException catch (e) {
        failures.add(e.path ?? entity.path);
      } catch (_) {
        failures.add(entity.path);
      }
    }
  }

  static bool hasEntry(String hash, {String? videoPath}) {
    final index = loadIndex();
    final entries = _entriesFromIndex(index);
    if (!entries.containsKey(hash) || !filesExist(hash)) return false;
    if (videoPath == null) return true;
    return File(videoPath).existsSync();
  }

  static Future<String?> findHashForUnchangedVideo(String videoPath) async {
    try {
      final file = File(videoPath);
      if (!await file.exists()) return null;
      final size = await file.length();
      final mtime = (await file.lastModified()).toIso8601String();
      final index = loadIndex();
      final entries = _entriesFromIndex(index);
      for (final item in entries.entries) {
        final value = item.value;
        if (value is! Map) continue;
        if (value['path'] != videoPath) continue;
        if ((value['size'] as num?)?.toInt() != size) continue;
        if (value['mtime'] != mtime) continue;
        if (!filesExist(item.key)) continue;
        return item.key;
      }
    } catch (_) {
      return null;
    }
    return null;
  }

  static Future<AnalysisCachePruneResult> enforceLimit({
    required int maxBytes,
    Set<String> protectedHashes = const {},
  }) async {
    return Isolate.run(
      () => _enforceLimitSync(
        maxBytes: maxBytes,
        protectedHashes: protectedHashes,
      ),
    );
  }

  static AnalysisCachePruneResult _enforceLimitSync({
    required int maxBytes,
    required Set<String> protectedHashes,
  }) {
    if (maxBytes <= 0) {
      return AnalysisCachePruneResult(
        snapshot: scan(maxBytes: maxBytes),
        deleteResult: const AnalysisCacheDeleteResult(
          deletedHashes: [],
          failuresByHash: {},
        ),
      );
    }

    var snapshot = scan(maxBytes: maxBytes);
    final deletedHashes = <String>[];
    final failuresByHash = <String, List<String>>{};

    while (snapshot.isOverLimit) {
      final candidates =
          snapshot.entries
              .where((entry) => !protectedHashes.contains(entry.hash))
              .toList()
            ..sort((a, b) {
              final aTime = a.lastAccessedAt ?? a.cachedAt ?? DateTime(0);
              final bTime = b.lastAccessedAt ?? b.cachedAt ?? DateTime(0);
              return aTime.compareTo(bTime);
            });
      if (candidates.isEmpty) break;

      final target = candidates.first;
      final result = _deleteEntriesSync([target.hash]);
      deletedHashes.addAll(result.deletedHashes);
      failuresByHash.addAll(result.failuresByHash);
      if (result.deletedHashes.isEmpty) break;
      snapshot = scan(maxBytes: maxBytes);
    }

    return AnalysisCachePruneResult(
      snapshot: snapshot,
      deleteResult: AnalysisCacheDeleteResult(
        deletedHashes: deletedHashes,
        failuresByHash: failuresByHash,
      ),
    );
  }

  static Future<int> currentBytesForHash(String hash) async {
    return Isolate.run(() => _currentBytesForHashSync(hash));
  }

  static Future<Map<String, int>> currentBytesByHash(
    Iterable<String> hashes,
  ) async {
    final uniqueHashes = hashes.where((hash) => hash.isNotEmpty).toSet();
    return Isolate.run(() {
      return {
        for (final hash in uniqueHashes) hash: _currentBytesForHashSync(hash),
      };
    });
  }

  static int _currentBytesForHashSync(String hash) {
    final dir = Directory(dataDir);
    if (!dir.existsSync()) return 0;
    var total = 0;
    final hashDirectory = Directory(hashDir(hash));
    if (hashDirectory.existsSync()) {
      for (final entity in hashDirectory.listSync(
        recursive: true,
        followLinks: false,
      )) {
        if (entity is! File) continue;
        try {
          total += entity.lengthSync();
        } catch (_) {
          // Best-effort stats.
        }
      }
    }
    for (final entity in dir.listSync(recursive: true, followLinks: false)) {
      if (entity is! File) continue;
      final name = p.basename(entity.path);
      if (!name.startsWith('$hash.')) continue;
      try {
        total += entity.lengthSync();
      } catch (_) {
        // Best-effort stats.
      }
    }
    return total;
  }

  static List<String> _pathsForHash(String hash) => [
    legacyAnalysisPath(hash),
    p.join(dataDir, '$hash.vbs4'),
    p.join(dataDir, '$hash.vbi'),
    p.join(dataDir, '$hash.vbt'),
    p.join(dataDir, '$hash.vbs2'),
    p.join(dataDir, '$hash.tmp.vbs4'),
    p.join(dataDir, '$hash.tmp.vbi'),
    p.join(dataDir, '$hash.tmp.vbt'),
    p.join(dataDir, '$hash.tmp.vvc'),
    hashDir(hash),
  ];

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

  static AnalysisCacheDeleteResult _deleteEntriesSync(Iterable<String> hashes) {
    return FileLockService.withExclusiveSync(_indexLockPath, () {
      final uniqueHashes = hashes.toSet();
      final index = _loadIndexUnlocked();
      final rawEntries = index['entries'];
      final entries = rawEntries is Map<String, dynamic>
          ? rawEntries
          : <String, dynamic>{};

      final deletedHashes = <String>[];
      final failuresByHash = <String, List<String>>{};

      for (final hash in uniqueHashes) {
        final failures = FileLockService.tryExclusiveSync(
          hashLockPath(hash),
          () => _deleteHashFilesSync(hash),
        );
        if (failures == null) {
          failuresByHash[hash] = [analysisPath(hash)];
          continue;
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
        _saveIndexUnlockedSync(index);
      }

      return AnalysisCacheDeleteResult(
        deletedHashes: deletedHashes,
        failuresByHash: failuresByHash,
      );
    });
  }

  static List<String> _deleteHashFilesSync(String hash) {
    final failures = <String>[];
    for (final path in _pathsForHash(hash)) {
      try {
        final type = FileSystemEntity.typeSync(path);
        if (type == FileSystemEntityType.directory) {
          Directory(path).deleteSync(recursive: true);
        } else if (type == FileSystemEntityType.file) {
          File(path).deleteSync();
        }
      } on FileSystemException catch (e) {
        failures.add(e.path ?? path);
      } catch (_) {
        failures.add(path);
      }
    }
    _deleteHashStagingDirsSync(hash, failures);
    return failures;
  }

  static void _deleteHashStagingDirsSync(String hash, List<String> failures) {
    final tmpDir = Directory(p.join(dataDir, 'tmp'));
    if (!tmpDir.existsSync()) return;
    for (final entity in tmpDir.listSync(followLinks: false)) {
      if (entity is! Directory) continue;
      final name = p.basename(entity.path);
      if (!name.startsWith('$hash.')) continue;
      try {
        entity.deleteSync(recursive: true);
      } on FileSystemException catch (e) {
        failures.add(e.path ?? entity.path);
      } catch (_) {
        failures.add(entity.path);
      }
    }
  }
}
