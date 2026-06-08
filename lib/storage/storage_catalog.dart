import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:sqlite3/sqlite3.dart';

import '../app_paths.dart';

class StorageCatalogThumbnail {
  final String path;
  final int bytes;

  const StorageCatalogThumbnail({required this.path, required this.bytes});
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
}
