import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:sqlite3/sqlite3.dart';

import '../app_paths.dart';

class StorageCatalogThumbnail {
  final String path;
  final int bytes;

  const StorageCatalogThumbnail({required this.path, required this.bytes});
}

class StorageCatalogMediaUsage {
  final String mediaHash;
  final String name;
  final String path;
  final int bytes;
  final int itemCount;
  final DateTime modifiedAt;
  final DateTime accessedAt;

  const StorageCatalogMediaUsage({
    required this.mediaHash,
    required this.name,
    required this.path,
    required this.bytes,
    required this.itemCount,
    required this.modifiedAt,
    required this.accessedAt,
  });
}

class StorageCatalogDeleteResult {
  final List<String> deletedMediaHashes;
  final Map<String, List<String>> failuresByMediaHash;

  const StorageCatalogDeleteResult({
    required this.deletedMediaHashes,
    required this.failuresByMediaHash,
  });
}

class StorageCatalog {
  static const int schemaVersion = 1;
  static const int markPayloadVersion = 1;
  static const int thumbnailCacheVersion = 1;

  final String databasePath;

  const StorageCatalog({required this.databasePath});

  factory StorageCatalog.defaultLocation() {
    return StorageCatalog(databasePath: AppPaths.current.storageDatabaseFile);
  }

  static String thumbnailDirectory({
    required String rootDir,
    required String mediaHash,
  }) {
    return p.join(rootDir, 'cache', mediaHash, 'mark_thumbnails');
  }

  void registerThumbnail({
    required String mediaHash,
    required int markId,
    required String renderDigest,
    required String path,
    required int bytes,
  }) {
    final db = open();
    try {
      final now = DateTime.now().millisecondsSinceEpoch;
      db.execute(
        'INSERT OR REPLACE INTO thumbnail_cache '
        '(media_hash, mark_id, render_digest, path, bytes, updated_at_ms, last_accessed_ms, cache_version) '
        'VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
        [
          mediaHash,
          markId,
          renderDigest,
          path,
          bytes,
          now,
          now,
          thumbnailCacheVersion,
        ],
      );
    } finally {
      db.close();
    }
  }

  StorageCatalogThumbnail? findThumbnail({
    required String mediaHash,
    required int markId,
    required String renderDigest,
  }) {
    final db = open();
    try {
      final rows = db.select(
        'SELECT path, bytes FROM thumbnail_cache '
        'WHERE media_hash = ? AND mark_id = ? AND render_digest = ? '
        'AND cache_version = ? '
        'LIMIT 1',
        [mediaHash, markId, renderDigest, thumbnailCacheVersion],
      );
      if (rows.isEmpty) return null;
      final row = rows.first;
      final path = row['path'] as String;
      if (!File(path).existsSync()) {
        db.execute(
          'DELETE FROM thumbnail_cache '
          'WHERE media_hash = ? AND mark_id = ? AND render_digest = ?',
          [mediaHash, markId, renderDigest],
        );
        return null;
      }
      db.execute(
        'UPDATE thumbnail_cache SET last_accessed_ms = ? '
        'WHERE media_hash = ? AND mark_id = ? AND render_digest = ?',
        [
          DateTime.now().millisecondsSinceEpoch,
          mediaHash,
          markId,
          renderDigest,
        ],
      );
      return StorageCatalogThumbnail(
        path: path,
        bytes: row['bytes'] as int? ?? File(path).lengthSync(),
      );
    } finally {
      db.close();
    }
  }

  List<StorageCatalogMediaUsage> listMarkDataUsage() {
    final db = open();
    try {
      final rows = db.select('''
        SELECT
          m.hash AS media_hash,
          m.name AS name,
          m.path AS path,
          SUM(LENGTH(k.payload_json)) AS bytes,
          COUNT(*) AS item_count,
          MAX(k.updated_at_ms) AS modified_ms,
          m.last_accessed_ms AS accessed_ms
        FROM marks k
        JOIN media m ON m.hash = k.media_hash
        GROUP BY m.hash
        ORDER BY modified_ms DESC
      ''');
      return [for (final row in rows) _usageFromRow(row)];
    } finally {
      db.close();
    }
  }

  List<StorageCatalogMediaUsage> listThumbnailUsage() {
    final db = open();
    try {
      final rows = db.select(
        '''
        SELECT
          t.media_hash AS media_hash,
          COALESCE(m.name, t.media_hash) AS name,
          COALESCE(m.path, '') AS path,
          SUM(t.bytes) AS bytes,
          COUNT(*) AS item_count,
          MAX(t.updated_at_ms) AS modified_ms,
          MAX(t.last_accessed_ms) AS accessed_ms
        FROM thumbnail_cache t
        LEFT JOIN media m ON m.hash = t.media_hash
        WHERE t.cache_version = ?
        GROUP BY t.media_hash
        ORDER BY modified_ms DESC
      ''',
        [thumbnailCacheVersion],
      );
      return [for (final row in rows) _usageFromRow(row)];
    } finally {
      db.close();
    }
  }

  StorageCatalogDeleteResult deleteMarksForMediaHashes(
    Iterable<String> mediaHashes,
  ) {
    final db = open();
    final deleted = <String>[];
    final failures = <String, List<String>>{};
    try {
      final uniqueHashes = mediaHashes.where(_isSafeMediaHash).toSet();
      for (final hash in mediaHashes) {
        if (!_isSafeMediaHash(hash)) failures[hash] = [hash];
      }
      for (final hash in uniqueHashes) {
        try {
          final failedPaths = _deleteThumbnailsForHash(db, hash);
          if (failedPaths.isNotEmpty) {
            failures[hash] = failedPaths;
            continue;
          }
          db.execute('DELETE FROM marks WHERE media_hash = ?', [hash]);
          deleted.add(hash);
        } catch (_) {
          failures[hash] = [hash];
        }
      }
      return StorageCatalogDeleteResult(
        deletedMediaHashes: deleted,
        failuresByMediaHash: failures,
      );
    } finally {
      db.close();
    }
  }

  StorageCatalogDeleteResult deleteThumbnailsForMediaHashes(
    Iterable<String> mediaHashes,
  ) {
    final db = open();
    final deleted = <String>[];
    final failures = <String, List<String>>{};
    try {
      final uniqueHashes = mediaHashes.where(_isSafeMediaHash).toSet();
      for (final hash in mediaHashes) {
        if (!_isSafeMediaHash(hash)) failures[hash] = [hash];
      }
      for (final hash in uniqueHashes) {
        final failedPaths = _deleteThumbnailsForHash(db, hash);
        if (failedPaths.isNotEmpty) {
          failures[hash] = failedPaths;
          continue;
        }
        deleted.add(hash);
      }
      return StorageCatalogDeleteResult(
        deletedMediaHashes: deleted,
        failuresByMediaHash: failures,
      );
    } finally {
      db.close();
    }
  }

  int reconcileMissingThumbnailFiles() {
    final db = open();
    try {
      final rows = db.select('SELECT path FROM thumbnail_cache');
      var deleted = 0;
      final delete = db.prepare('DELETE FROM thumbnail_cache WHERE path = ?');
      try {
        for (final row in rows) {
          final path = row['path'] as String;
          if (File(path).existsSync()) continue;
          delete.execute([path]);
          deleted++;
        }
      } finally {
        delete.close();
      }
      return deleted;
    } finally {
      db.close();
    }
  }

  Database open() {
    Directory(p.dirname(databasePath)).createSync(recursive: true);
    final db = sqlite3.open(databasePath);
    db.execute('PRAGMA foreign_keys = ON');
    db.execute('PRAGMA journal_mode = WAL');
    ensureSchema(db);
    return db;
  }

  static void ensureSchema(Database db) {
    db.execute('''
      CREATE TABLE IF NOT EXISTS schema_meta (
        key TEXT PRIMARY KEY,
        value TEXT NOT NULL
      )
    ''');
    db.execute('''
      CREATE TABLE IF NOT EXISTS media (
        hash TEXT PRIMARY KEY,
        media_id TEXT NOT NULL,
        path TEXT NOT NULL,
        name TEXT NOT NULL,
        size INTEGER NOT NULL DEFAULT 0,
        mtime_ms INTEGER NOT NULL DEFAULT 0,
        first_seen_ms INTEGER NOT NULL,
        last_accessed_ms INTEGER NOT NULL
      )
    ''');
    db.execute('''
      CREATE TABLE IF NOT EXISTS marks (
        media_hash TEXT NOT NULL REFERENCES media(hash) ON DELETE CASCADE,
        mark_id INTEGER NOT NULL,
        payload_json TEXT NOT NULL,
        updated_at_ms INTEGER NOT NULL,
        PRIMARY KEY (media_hash, mark_id)
      )
    ''');
    db.execute('''
      CREATE TABLE IF NOT EXISTS thumbnail_cache (
        media_hash TEXT NOT NULL,
        mark_id INTEGER NOT NULL,
        render_digest TEXT NOT NULL,
        path TEXT NOT NULL,
        bytes INTEGER NOT NULL DEFAULT 0,
        updated_at_ms INTEGER NOT NULL,
        last_accessed_ms INTEGER NOT NULL,
        cache_version INTEGER NOT NULL DEFAULT 1,
        PRIMARY KEY (media_hash, mark_id, render_digest)
      )
    ''');
    db.execute('INSERT OR REPLACE INTO schema_meta(key, value) VALUES (?, ?)', [
      'schema_version',
      '$schemaVersion',
    ]);
    db.execute('INSERT OR REPLACE INTO schema_meta(key, value) VALUES (?, ?)', [
      'mark_payload_version',
      '$markPayloadVersion',
    ]);
    db.execute('INSERT OR REPLACE INTO schema_meta(key, value) VALUES (?, ?)', [
      'thumbnail_cache_version',
      '$thumbnailCacheVersion',
    ]);
  }

  static StorageCatalogMediaUsage _usageFromRow(Row row) {
    final hash = row['media_hash'] as String;
    final name = row['name'] as String? ?? hash;
    final path = row['path'] as String? ?? '';
    return StorageCatalogMediaUsage(
      mediaHash: hash,
      name: name.isEmpty ? hash : name,
      path: path,
      bytes: _readInt(row['bytes']),
      itemCount: _readInt(row['item_count']),
      modifiedAt: _dateFromMs(_readInt(row['modified_ms'])),
      accessedAt: _dateFromMs(_readInt(row['accessed_ms'])),
    );
  }

  static List<String> _deleteThumbnailsForHash(Database db, String hash) {
    final failedPaths = <String>[];
    final rows = db.select(
      'SELECT path FROM thumbnail_cache WHERE media_hash = ?',
      [hash],
    );
    for (final row in rows) {
      final path = row['path'] as String;
      try {
        final file = File(path);
        if (file.existsSync()) file.deleteSync();
      } on FileSystemException catch (e) {
        failedPaths.add(e.path ?? path);
      } catch (_) {
        failedPaths.add(path);
      }
    }
    if (failedPaths.isEmpty) {
      db.execute('DELETE FROM thumbnail_cache WHERE media_hash = ?', [hash]);
    }
    return failedPaths;
  }

  static int _readInt(Object? value) {
    if (value is int) return value;
    if (value is num) return value.toInt();
    return 0;
  }

  static DateTime _dateFromMs(int milliseconds) {
    if (milliseconds <= 0) return DateTime.fromMillisecondsSinceEpoch(0);
    return DateTime.fromMillisecondsSinceEpoch(milliseconds);
  }

  static bool _isSafeMediaHash(String hash) {
    if (hash.isEmpty || hash.length > 128) return false;
    return RegExp(r'^[a-zA-Z0-9_-]+$').hasMatch(hash);
  }
}
