import 'dart:io';

import 'package:path/path.dart' as p;

import '../analysis/analysis_cache.dart';
import '../app_log.dart';
import '../app_paths.dart';
import 'storage_catalog.dart';

class StorageFileEntry {
  final String id;
  final String name;
  final String path;
  final String? subtitle;
  final int bytes;
  final int itemCount;
  final DateTime modifiedAt;
  final DateTime accessedAt;

  const StorageFileEntry({
    required this.id,
    required this.name,
    required this.path,
    this.subtitle,
    required this.bytes,
    this.itemCount = 1,
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
    final databasePath = p.join(rootDir, 'storage.sqlite');
    if (await File(databasePath).exists()) {
      try {
        StorageCatalog(
          databasePath: databasePath,
        ).reconcileMissingThumbnailFiles();
      } catch (error, stack) {
        // The settings page should keep opening if a developer manually leaves
        // an old or corrupt catalog behind.
        log.warning('mark storage catalog reconciliation failed', error, stack);
      }
    }
    final catalog = File(databasePath).existsSync()
        ? StorageCatalog(databasePath: databasePath)
        : null;
    final annotations = await _scanMarkDataByMedia(
      rootDir,
      catalog: catalog,
      maxBytes: 0,
    );
    final thumbnails = await _scanMarkThumbnailsByMedia(
      rootDir,
      catalog: catalog,
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
    final databasePath = p.join(resolvedRoot, 'storage.sqlite');
    if (!File(databasePath).existsSync()) {
      return Future.value(
        StorageDeleteResult(deletedIds: ids.toList(), failuresById: const {}),
      );
    }
    final result = StorageCatalog(
      databasePath: databasePath,
    ).deleteMarksForMediaHashes(ids);
    return Future.value(_deleteResultFromCatalog(result));
  }

  static Future<StorageDeleteResult> deleteMarkThumbnailFiles(
    Iterable<String> ids, {
    String? rootDir,
  }) {
    final resolvedRoot = rootDir ?? AppPaths.current.rootDir;
    final databasePath = p.join(resolvedRoot, 'storage.sqlite');
    if (!File(databasePath).existsSync()) {
      return Future.value(
        StorageDeleteResult(deletedIds: ids.toList(), failuresById: const {}),
      );
    }
    final result = StorageCatalog(
      databasePath: databasePath,
    ).deleteThumbnailsForMediaHashes(ids);
    return Future.value(_deleteResultFromCatalog(result));
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
    final before = await _scanMarkThumbnailsByMedia(
      resolvedRoot,
      catalog: File(p.join(resolvedRoot, 'storage.sqlite')).existsSync()
          ? StorageCatalog(databasePath: p.join(resolvedRoot, 'storage.sqlite'))
          : null,
      maxBytes: 0,
    );
    final cacheDir = Directory(p.join(resolvedRoot, 'cache'));
    if (await cacheDir.exists()) {
      await for (final entity in cacheDir.list(
        recursive: true,
        followLinks: false,
      )) {
        if (entity is! Directory ||
            p.basename(entity.path) != 'mark_thumbnails') {
          continue;
        }
        try {
          await entity.delete(recursive: true);
        } on FileSystemException {
          // Ignore directories that disappear during settings refresh.
        }
      }
    }
    final databasePath = p.join(resolvedRoot, 'storage.sqlite');
    if (await File(databasePath).exists()) {
      try {
        StorageCatalog(
          databasePath: databasePath,
        ).reconcileMissingThumbnailFiles();
      } catch (error, stack) {
        // Keep file cleanup independent from catalog health.
        log.warning(
          'mark thumbnail catalog reconciliation failed after cleanup',
          error,
          stack,
        );
      }
    }
    return MarkThumbnailClearResult(
      deletedBytes: before.totalBytes,
      deletedFileCount: before.fileCount,
    );
  }

  static Future<StorageFolderSnapshot> _scanMarkDataByMedia(
    String rootDir, {
    required StorageCatalog? catalog,
    required int maxBytes,
  }) async {
    final entries = catalog == null
        ? const <StorageFileEntry>[]
        : _entriesFromMediaUsage(catalog.listMarkDataUsage());
    final bytes = entries.fold<int>(0, (sum, entry) => sum + entry.bytes);
    return StorageFolderSnapshot(
      path: rootDir,
      totalBytes: bytes,
      fileCount: entries.fold<int>(0, (sum, entry) => sum + entry.itemCount),
      maxBytes: maxBytes,
      entries: entries,
    );
  }

  static Future<StorageFolderSnapshot> _scanMarkThumbnailsByMedia(
    String rootDir, {
    required StorageCatalog? catalog,
    required int maxBytes,
  }) async {
    final cacheDir = p.join(rootDir, 'cache');
    if (catalog == null) {
      return StorageFolderSnapshot(
        path: cacheDir,
        totalBytes: 0,
        fileCount: 0,
        maxBytes: maxBytes,
        entries: const [],
      );
    }
    final entries = _entriesFromMediaUsage(catalog.listThumbnailUsage());
    final bytes = entries.fold<int>(0, (sum, entry) => sum + entry.bytes);
    return StorageFolderSnapshot(
      path: cacheDir,
      totalBytes: bytes,
      fileCount: entries.fold<int>(0, (sum, entry) => sum + entry.itemCount),
      maxBytes: maxBytes,
      entries: entries,
    );
  }

  static List<StorageFileEntry> _entriesFromMediaUsage(
    List<StorageCatalogMediaUsage> usage,
  ) {
    final nameCounts = <String, int>{};
    final pathCounts = <String, int>{};
    for (final item in usage) {
      nameCounts[item.name] = (nameCounts[item.name] ?? 0) + 1;
      if (item.path.isNotEmpty) {
        pathCounts[item.path] = (pathCounts[item.path] ?? 0) + 1;
      }
    }
    final entries = [
      for (final item in usage)
        StorageFileEntry(
          id: item.mediaHash,
          name: _displayNameForMedia(
            item,
            nameCounts: nameCounts,
            pathCounts: pathCounts,
          ),
          path: item.path.isNotEmpty ? item.path : item.mediaHash,
          subtitle: item.path.isNotEmpty ? item.path : null,
          bytes: item.bytes,
          itemCount: item.itemCount,
          modifiedAt: item.modifiedAt,
          accessedAt: item.accessedAt,
        ),
    ];
    entries.sort((a, b) => b.lruTime.compareTo(a.lruTime));
    return entries;
  }

  static String _displayNameForMedia(
    StorageCatalogMediaUsage item, {
    required Map<String, int> nameCounts,
    required Map<String, int> pathCounts,
  }) {
    final duplicateName = (nameCounts[item.name] ?? 0) > 1;
    final duplicatePath =
        item.path.isNotEmpty && (pathCounts[item.path] ?? 0) > 1;
    final name = item.name.isNotEmpty ? item.name : item.mediaHash;
    if (!duplicateName && !duplicatePath) return name;
    return '$name #${_shortHash(item.mediaHash)}';
  }

  static String _shortHash(String hash) {
    return hash.length <= 6 ? hash : hash.substring(0, 6);
  }

  static StorageDeleteResult _deleteResultFromCatalog(
    StorageCatalogDeleteResult result,
  ) {
    return StorageDeleteResult(
      deletedIds: result.deletedMediaHashes,
      failuresById: result.failuresByMediaHash,
    );
  }
}
