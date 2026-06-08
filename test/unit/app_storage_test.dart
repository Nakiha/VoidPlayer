import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:void_player/storage/app_storage.dart';
import 'package:void_player/storage/storage_catalog.dart';

void main() {
  test('scans mark data and thumbnails grouped by media', () async {
    final root = await Directory.systemTemp.createTemp('void_storage_test_');
    addTearDown(() => root.delete(recursive: true));

    final catalog = _catalog(root.path);
    _insertMedia(catalog, hash: 'media_hash', path: '/media/a.mp4');
    _insertMark(catalog, mediaHash: 'media_hash', markId: 1);
    final thumbnail = await _createThumbnail(
      root.path,
      mediaHash: 'media_hash',
      markId: 1,
      bytes: 3,
      catalog: catalog,
    );

    final snapshot = await AppStorage.scanMarkStorage(rootDir: root.path);

    expect(snapshot.annotationBytes, greaterThan(0));
    expect(snapshot.annotationFileCount, 1);
    expect(snapshot.annotations.entries.single.id, 'media_hash');
    expect(snapshot.annotations.entries.single.name, 'a.mp4');
    expect(snapshot.annotations.entries.single.path, '/media/a.mp4');
    expect(snapshot.thumbnailBytes, 3);
    expect(snapshot.thumbnailFileCount, 1);
    expect(snapshot.thumbnails.entries.single.id, 'media_hash');
    expect(thumbnail.existsSync(), isTrue);
    expect(
      snapshot.totalBytes,
      snapshot.annotationBytes + snapshot.thumbnailBytes,
    );
  });

  test('shows a short hash for duplicate media names', () async {
    final root = await Directory.systemTemp.createTemp(
      'void_storage_duplicates_',
    );
    addTearDown(() => root.delete(recursive: true));

    final catalog = _catalog(root.path);
    _insertMedia(catalog, hash: 'aaaaaa111', path: '/media/one/clip.mp4');
    _insertMedia(catalog, hash: 'bbbbbb222', path: '/media/two/clip.mp4');
    _insertMark(catalog, mediaHash: 'aaaaaa111', markId: 1);
    _insertMark(catalog, mediaHash: 'bbbbbb222', markId: 1);

    final snapshot = await AppStorage.scanMarkStorage(rootDir: root.path);

    expect(snapshot.annotations.entries.map((entry) => entry.name).toSet(), {
      'clip.mp4 #aaaaaa',
      'clip.mp4 #bbbbbb',
    });
  });

  test('clears thumbnails without deleting mark data', () async {
    final root = await Directory.systemTemp.createTemp('void_storage_clear_');
    addTearDown(() => root.delete(recursive: true));

    final catalog = _catalog(root.path);
    _insertMedia(catalog, hash: 'media_hash', path: '/media/a.mp4');
    _insertMark(catalog, mediaHash: 'media_hash', markId: 1);
    final thumbnail = await _createThumbnail(
      root.path,
      mediaHash: 'media_hash',
      markId: 1,
      bytes: 3,
      catalog: catalog,
    );

    final result = await AppStorage.clearMarkThumbnails(rootDir: root.path);

    expect(result.deletedBytes, 3);
    expect(result.deletedFileCount, 1);
    expect(thumbnail.existsSync(), isFalse);

    final snapshot = await AppStorage.scanMarkStorage(rootDir: root.path);
    expect(snapshot.annotationBytes, greaterThan(0));
    expect(snapshot.thumbnailBytes, 0);
  });

  test('deletes selected media mark data and derived thumbnails', () async {
    final root = await Directory.systemTemp.createTemp(
      'void_storage_delete_marks_',
    );
    addTearDown(() => root.delete(recursive: true));

    final catalog = _catalog(root.path);
    _insertMedia(catalog, hash: 'media_hash', path: '/media/a.mp4');
    _insertMark(catalog, mediaHash: 'media_hash', markId: 1);
    final thumbnail = await _createThumbnail(
      root.path,
      mediaHash: 'media_hash',
      markId: 1,
      bytes: 3,
      catalog: catalog,
    );

    final result = await AppStorage.deleteMarkAnnotationFiles([
      'media_hash',
    ], rootDir: root.path);

    expect(result.deletedIds, ['media_hash']);
    expect(result.hasFailures, isFalse);
    expect(thumbnail.existsSync(), isFalse);

    final snapshot = await AppStorage.scanMarkStorage(rootDir: root.path);
    expect(snapshot.annotationBytes, 0);
    expect(snapshot.thumbnailBytes, 0);
  });

  test(
    'enforces thumbnail cache limit by removing least recently used media',
    () async {
      final root = await Directory.systemTemp.createTemp(
        'void_storage_lru_thumbs_',
      );
      addTearDown(() => root.delete(recursive: true));

      final catalog = _catalog(root.path);
      _insertMedia(catalog, hash: 'old_hash', path: '/media/old.mp4');
      _insertMedia(catalog, hash: 'new_hash', path: '/media/new.mp4');
      final oldThumbnail = await _createThumbnail(
        root.path,
        mediaHash: 'old_hash',
        markId: 1,
        bytes: 4,
        catalog: catalog,
      );
      final newThumbnail = await _createThumbnail(
        root.path,
        mediaHash: 'new_hash',
        markId: 1,
        bytes: 4,
        catalog: catalog,
      );
      _setThumbnailTimes(
        catalog,
        mediaHash: 'old_hash',
        accessedAt: DateTime(2024).millisecondsSinceEpoch,
      );
      _setThumbnailTimes(
        catalog,
        mediaHash: 'new_hash',
        accessedAt: DateTime(2025).millisecondsSinceEpoch,
      );

      final result = await AppStorage.enforceMarkThumbnailLimit(
        maxBytes: 5,
        rootDir: root.path,
      );

      expect(result.deleteResult.deletedIds, ['old_hash']);
      expect(oldThumbnail.existsSync(), isFalse);
      expect(newThumbnail.existsSync(), isTrue);
      expect(result.snapshot.thumbnailBytes, 4);
    },
  );
}

StorageCatalog _catalog(String rootPath) {
  return StorageCatalog(databasePath: p.join(rootPath, 'storage.sqlite'));
}

void _insertMedia(
  StorageCatalog catalog, {
  required String hash,
  required String path,
}) {
  final db = catalog.open();
  try {
    final now = DateTime.now().millisecondsSinceEpoch;
    db.execute(
      'INSERT OR REPLACE INTO media '
      '(hash, media_id, path, name, size, mtime_ms, first_seen_ms, last_accessed_ms) '
      'VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
      [hash, path, path, p.basename(path), 0, now, now, now],
    );
  } finally {
    db.close();
  }
}

void _insertMark(
  StorageCatalog catalog, {
  required String mediaHash,
  required int markId,
}) {
  final db = catalog.open();
  try {
    db.execute(
      'INSERT OR REPLACE INTO marks '
      '(media_hash, mark_id, payload_json, updated_at_ms) '
      'VALUES (?, ?, ?, ?)',
      [
        mediaHash,
        markId,
        '{"version":1,"shape":"rect","text":"note"}',
        DateTime.now().millisecondsSinceEpoch,
      ],
    );
  } finally {
    db.close();
  }
}

Future<File> _createThumbnail(
  String rootPath, {
  required String mediaHash,
  required int markId,
  required int bytes,
  required StorageCatalog catalog,
}) async {
  final thumbnail = File(
    p.join(
      rootPath,
      'cache',
      mediaHash,
      'mark_thumbnails',
      'mark_${markId}_digest.png',
    ),
  );
  await thumbnail.create(recursive: true);
  await thumbnail.writeAsBytes(List<int>.filled(bytes, markId));
  catalog.registerThumbnail(
    mediaHash: mediaHash,
    markId: markId,
    renderDigest: 'digest',
    path: thumbnail.path,
    bytes: bytes,
  );
  return thumbnail;
}

void _setThumbnailTimes(
  StorageCatalog catalog, {
  required String mediaHash,
  required int accessedAt,
}) {
  final db = catalog.open();
  try {
    db.execute(
      'UPDATE thumbnail_cache '
      'SET updated_at_ms = ?, last_accessed_ms = ? '
      'WHERE media_hash = ?',
      [accessedAt, accessedAt, mediaHash],
    );
  } finally {
    db.close();
  }
}
