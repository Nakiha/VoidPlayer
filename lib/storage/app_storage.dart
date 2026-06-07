import 'dart:io';

import 'package:path/path.dart' as p;

import '../analysis/analysis_cache.dart';
import '../app_paths.dart';

class StorageFileEntry {
  final String id;
  final String name;
  final String path;
  final int bytes;
  final DateTime modifiedAt;
  final DateTime accessedAt;

  const StorageFileEntry({
    required this.id,
    required this.name,
    required this.path,
    required this.bytes,
    required this.modifiedAt,
    required this.accessedAt,
  });

  DateTime get lruTime =>
      accessedAt.isAfter(DateTime(1971)) ? accessedAt : modifiedAt;
}

class StorageDeleteResult {
  final List<String> deletedIds;
  final Map<String, List<String>> failuresById;

  const StorageDeleteResult({
    required this.deletedIds,
    required this.failuresById,
  });

  int get deletedCount => deletedIds.length;
  int get failedCount => failuresById.length;
  bool get hasFailures => failuresById.isNotEmpty;
}

class StoragePruneResult {
  final MarkStorageSnapshot snapshot;
  final StorageDeleteResult deleteResult;

  const StoragePruneResult({
    required this.snapshot,
    required this.deleteResult,
  });
}

class StorageFolderSnapshot {
  final String path;
  final int totalBytes;
  final int fileCount;
  final int maxBytes;
  final List<StorageFileEntry> entries;

  const StorageFolderSnapshot({
    required this.path,
    required this.totalBytes,
    required this.fileCount,
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

class MarkStorageSnapshot {
  final StorageFolderSnapshot annotations;
  final StorageFolderSnapshot thumbnails;

  const MarkStorageSnapshot({
    required this.annotations,
    required this.thumbnails,
  });

  String get annotationsPath => annotations.path;
  String get thumbnailsPath => thumbnails.path;
  int get annotationBytes => annotations.totalBytes;
  int get annotationFileCount => annotations.fileCount;
  int get thumbnailBytes => thumbnails.totalBytes;
  int get thumbnailFileCount => thumbnails.fileCount;
  int get totalBytes => annotationBytes + thumbnailBytes;
  bool get hasAnnotationData => annotationBytes > 0;
  bool get hasThumbnails => thumbnailBytes > 0;
}

class AppStorageSnapshot {
  final String rootPath;
  final AnalysisCacheSnapshot analysis;
  final MarkStorageSnapshot marks;

  const AppStorageSnapshot({
    required this.rootPath,
    required this.analysis,
    required this.marks,
  });

  int get rebuildableCacheBytes => analysis.totalBytes;
  int get annotationDataBytes => marks.annotationBytes;
  int get annotationThumbnailBytes => marks.thumbnailBytes;
  int get totalBytes =>
      rebuildableCacheBytes + annotationDataBytes + annotationThumbnailBytes;
}

class MarkThumbnailClearResult {
  final int deletedBytes;
  final int deletedFileCount;

  const MarkThumbnailClearResult({
    required this.deletedBytes,
    required this.deletedFileCount,
  });
}

class AppStorage {
  const AppStorage._();

  static Future<AppStorageSnapshot> snapshot({
    int analysisMaxBytes = 0,
    int markThumbnailMaxBytes = 0,
  }) async {
    final paths = AppPaths.current;
    final analysis = await AnalysisCache.snapshot(maxBytes: analysisMaxBytes);
    final marks = await scanMarkStorage(
      rootDir: paths.rootDir,
      thumbnailMaxBytes: markThumbnailMaxBytes,
    );
    return AppStorageSnapshot(
      rootPath: paths.rootDir,
      analysis: analysis,
      marks: marks,
    );
  }

  static Future<MarkStorageSnapshot> scanMarkStorage({
    required String rootDir,
    int thumbnailMaxBytes = 0,
  }) async {
    final annotationsPath = p.join(rootDir, 'annotations');
    final thumbnailsPath = p.join(rootDir, 'marks', 'thumbnails');
    final annotations = await _scanDirectory(
      Directory(annotationsPath),
      maxBytes: 0,
    );
    final thumbnails = await _scanDirectory(
      Directory(thumbnailsPath),
      maxBytes: thumbnailMaxBytes,
    );
    return MarkStorageSnapshot(
      annotations: annotations,
      thumbnails: thumbnails,
    );
  }

  static Future<StorageDeleteResult> deleteMarkAnnotationFiles(
    Iterable<String> ids, {
    String? rootDir,
  }) {
    final resolvedRoot = rootDir ?? AppPaths.current.rootDir;
    return _deleteFilesInDirectory(
      ids,
      directory: p.join(resolvedRoot, 'annotations'),
    );
  }

  static Future<StorageDeleteResult> deleteMarkThumbnailFiles(
    Iterable<String> ids, {
    String? rootDir,
  }) {
    final resolvedRoot = rootDir ?? AppPaths.current.rootDir;
    return _deleteFilesInDirectory(
      ids,
      directory: p.join(resolvedRoot, 'marks', 'thumbnails'),
    );
  }

  static Future<StoragePruneResult> enforceMarkThumbnailLimit({
    required int maxBytes,
    String? rootDir,
  }) async {
    final resolvedRoot = rootDir ?? AppPaths.current.rootDir;
    var snapshot = await scanMarkStorage(
      rootDir: resolvedRoot,
      thumbnailMaxBytes: maxBytes,
    );
    if (maxBytes <= 0 || !snapshot.thumbnails.isOverLimit) {
      return StoragePruneResult(
        snapshot: snapshot,
        deleteResult: const StorageDeleteResult(
          deletedIds: [],
          failuresById: {},
        ),
      );
    }

    final deletedIds = <String>[];
    final failuresById = <String, List<String>>{};
    while (snapshot.thumbnails.isOverLimit) {
      final candidates = snapshot.thumbnails.entries.toList()
        ..sort((a, b) => a.lruTime.compareTo(b.lruTime));
      if (candidates.isEmpty) break;

      final target = candidates.first;
      final result = await deleteMarkThumbnailFiles([
        target.id,
      ], rootDir: resolvedRoot);
      deletedIds.addAll(result.deletedIds);
      failuresById.addAll(result.failuresById);
      if (result.deletedIds.isEmpty) break;
      snapshot = await scanMarkStorage(
        rootDir: resolvedRoot,
        thumbnailMaxBytes: maxBytes,
      );
    }

    return StoragePruneResult(
      snapshot: snapshot,
      deleteResult: StorageDeleteResult(
        deletedIds: deletedIds,
        failuresById: failuresById,
      ),
    );
  }

  static Future<MarkThumbnailClearResult> clearMarkThumbnails({
    String? rootDir,
  }) async {
    final resolvedRoot = rootDir ?? AppPaths.current.rootDir;
    final thumbnailsDir = Directory(
      p.join(resolvedRoot, 'marks', 'thumbnails'),
    );
    final before = await _scanDirectory(thumbnailsDir, maxBytes: 0);
    if (await thumbnailsDir.exists()) {
      await thumbnailsDir.delete(recursive: true);
    }
    await thumbnailsDir.create(recursive: true);
    return MarkThumbnailClearResult(
      deletedBytes: before.totalBytes,
      deletedFileCount: before.fileCount,
    );
  }

  static Future<StorageFolderSnapshot> _scanDirectory(
    Directory directory, {
    required int maxBytes,
  }) async {
    if (!await directory.exists()) {
      return StorageFolderSnapshot(
        path: directory.path,
        totalBytes: 0,
        fileCount: 0,
        maxBytes: maxBytes,
        entries: const [],
      );
    }
    var bytes = 0;
    final entries = <StorageFileEntry>[];
    await for (final entity in directory.list(
      recursive: true,
      followLinks: false,
    )) {
      if (entity is! File) continue;
      try {
        final stat = await entity.stat();
        bytes += stat.size;
        entries.add(
          StorageFileEntry(
            id: p.relative(entity.path, from: directory.path),
            name: p.basename(entity.path),
            path: entity.path,
            bytes: stat.size,
            modifiedAt: stat.modified,
            accessedAt: stat.accessed,
          ),
        );
      } on FileSystemException {
        // Ignore files that disappear while the settings page is refreshing.
      }
    }
    entries.sort((a, b) => b.lruTime.compareTo(a.lruTime));
    return StorageFolderSnapshot(
      path: directory.path,
      totalBytes: bytes,
      fileCount: entries.length,
      maxBytes: maxBytes,
      entries: entries,
    );
  }

  static Future<StorageDeleteResult> _deleteFilesInDirectory(
    Iterable<String> ids, {
    required String directory,
  }) async {
    final uniqueIds = ids.toSet();
    final deletedIds = <String>[];
    final failuresById = <String, List<String>>{};
    for (final id in uniqueIds) {
      if (!_isSafeRelativeFileId(id)) {
        failuresById[id] = [id];
        continue;
      }
      final file = File(p.join(directory, id));
      try {
        if (await file.exists()) {
          await file.delete();
        }
        deletedIds.add(id);
      } on FileSystemException catch (e) {
        failuresById[id] = [e.path ?? file.path];
      } catch (_) {
        failuresById[id] = [file.path];
      }
    }
    return StorageDeleteResult(
      deletedIds: deletedIds,
      failuresById: failuresById,
    );
  }

  static bool _isSafeRelativeFileId(String id) {
    if (id.isEmpty || p.isAbsolute(id)) return false;
    final parts = p.split(p.normalize(id));
    return !parts.contains('..');
  }
}
