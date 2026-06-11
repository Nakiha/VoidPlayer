import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:sqlite3/sqlite3.dart';
import 'package:void_player/storage/storage_catalog.dart';

void main() {
  test(
    'finds registered thumbnail and prunes missing thumbnail files',
    () async {
      final root = await Directory.systemTemp.createTemp(
        'void_storage_catalog_test_',
      );
      addTearDown(() => root.delete(recursive: true));

      final catalog = StorageCatalog(
        databasePath: p.join(root.path, 'storage.sqlite'),
      );
      final thumbnail = File(
        p.join(root.path, 'cache', 'media_hash', 'mark_thumbnails', 'a.png'),
      );
      await thumbnail.create(recursive: true);
      await thumbnail.writeAsBytes([1, 2, 3]);

      catalog.registerThumbnail(
        mediaHash: 'media_hash',
        markId: 7,
        renderDigest: 'digest',
        path: thumbnail.path,
        bytes: 3,
      );

      final found = catalog.findThumbnail(
        mediaHash: 'media_hash',
        markId: 7,
        renderDigest: 'digest',
      );
      expect(found?.path, thumbnail.path);
      expect(found?.bytes, 3);

      await thumbnail.delete();

      expect(
        catalog.findThumbnail(
          mediaHash: 'media_hash',
          markId: 7,
          renderDigest: 'digest',
        ),
        isNull,
      );
      expect(catalog.reconcileMissingThumbnailFiles(), 0);
    },
  );

  test('lists media grouped mark and thumbnail usage', () async {
    final root = await Directory.systemTemp.createTemp(
      'void_storage_catalog_usage_',
    );
    addTearDown(() => root.delete(recursive: true));

    final catalog = StorageCatalog(
      databasePath: p.join(root.path, 'storage.sqlite'),
    );
    final db = catalog.open();
    try {
      final now = DateTime.now().millisecondsSinceEpoch;
      db.execute(
        'INSERT INTO media '
        '(hash, media_id, path, name, size, mtime_ms, first_seen_ms, last_accessed_ms) '
        'VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
        [
          'media_hash',
          '/media/a.mp4',
          '/media/a.mp4',
          'a.mp4',
          0,
          now,
          now,
          now,
        ],
      );
      db.execute(
        'INSERT INTO marks '
        '(media_hash, mark_id, payload_json, updated_at_ms) '
        'VALUES (?, ?, ?, ?)',
        ['media_hash', 1, '{"version":1}', now],
      );
    } finally {
      db.close();
    }

    final thumbnail = File(
      p.join(root.path, 'cache', 'media_hash', 'mark_thumbnails', 'a.png'),
    );
    await thumbnail.create(recursive: true);
    await thumbnail.writeAsBytes([1, 2, 3]);
    catalog.registerThumbnail(
      mediaHash: 'media_hash',
      markId: 1,
      renderDigest: 'digest',
      path: thumbnail.path,
      bytes: 3,
    );

    final markUsage = catalog.listMarkDataUsage();
    final thumbnailUsage = catalog.listThumbnailUsage();

    expect(markUsage.single.mediaHash, 'media_hash');
    expect(markUsage.single.name, 'a.mp4');
    expect(markUsage.single.path, '/media/a.mp4');
    expect(markUsage.single.itemCount, 1);
    expect(thumbnailUsage.single.mediaHash, 'media_hash');
    expect(thumbnailUsage.single.bytes, 3);
  });

  test('migrates v1 media table to add source_id and stores lineage', () async {
    final root = await Directory.systemTemp.createTemp(
      'void_player_storage_catalog_migration_test_',
    );
    addTearDown(() async {
      if (await root.exists()) await root.delete(recursive: true);
    });
    final dbPath = p.join(root.path, 'v1.sqlite');
    final db = sqlite3.open(dbPath);
    try {
      db.execute('''
        CREATE TABLE media (
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
      db.execute(
        'INSERT INTO media (hash, media_id, path, name, first_seen_ms, last_accessed_ms) '
        "VALUES ('abc', 'id', '/x', 'x', 0, 0)",
      );
    } finally {
      db.close();
    }

    final catalog = StorageCatalog(databasePath: dbPath);
    expect(catalog.sourceIdForMediaHash('abc'), isNull);

    catalog.setMediaSourceId(mediaHash: 'abc', sourceId: 'clip01');
    expect(catalog.sourceIdForMediaHash('abc'), 'clip01');
    expect(catalog.mediaHashesForSourceId('clip01'), ['abc']);

    catalog.setMediaSourceId(mediaHash: 'abc', sourceId: null);
    expect(catalog.sourceIdForMediaHash('abc'), isNull);
  });
}
