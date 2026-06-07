import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:void_player/storage/app_storage.dart';
import 'package:void_player/storage/storage_catalog.dart';

void main() {
  test('scans annotation source data and thumbnail cache separately', () async {
    final root = await Directory.systemTemp.createTemp('void_storage_test_');
    addTearDown(() => root.delete(recursive: true));

    final annotation = await _createStorageDatabase(root.path);

    final thumbnail = File(
      p.join(root.path, 'cache', 'media_hash', 'mark_thumbnails', 'a.png'),
    );
    await thumbnail.create(recursive: true);
    await thumbnail.writeAsBytes([5, 6, 7]);

    final snapshot = await AppStorage.scanMarkStorage(rootDir: root.path);

    expect(snapshot.annotationBytes, annotation.lengthSync());
    expect(snapshot.annotationFileCount, 1);
    expect(snapshot.thumbnailBytes, 3);
    expect(snapshot.thumbnailFileCount, 1);
    expect(
      snapshot.totalBytes,
      snapshot.annotationBytes + snapshot.thumbnailBytes,
    );
  });

  test('clears thumbnails without deleting annotation source data', () async {
    final root = await Directory.systemTemp.createTemp('void_storage_clear_');
    addTearDown(() => root.delete(recursive: true));

    final annotation = await _createStorageDatabase(root.path);

    final thumbnail = File(
      p.join(root.path, 'cache', 'media_hash', 'mark_thumbnails', 'a.png'),
    );
    await thumbnail.create(recursive: true);
    await thumbnail.writeAsBytes([5, 6, 7]);

    final result = await AppStorage.clearMarkThumbnails(rootDir: root.path);

    expect(result.deletedBytes, 3);
    expect(result.deletedFileCount, 1);
    expect(annotation.existsSync(), isTrue);
    expect(thumbnail.existsSync(), isFalse);

    final snapshot = await AppStorage.scanMarkStorage(rootDir: root.path);
    expect(snapshot.annotationBytes, annotation.lengthSync());
    expect(snapshot.thumbnailBytes, 0);
  });

  test('deletes selected mark database files only', () async {
    final root = await Directory.systemTemp.createTemp(
      'void_storage_delete_marks_',
    );
    addTearDown(() => root.delete(recursive: true));

    final annotation = await _createStorageDatabase(root.path);

    final thumbnail = File(
      p.join(root.path, 'cache', 'media_hash', 'mark_thumbnails', 'a.png'),
    );
    await thumbnail.create(recursive: true);
    await thumbnail.writeAsBytes([5, 6, 7]);

    final result = await AppStorage.deleteMarkAnnotationFiles([
      'storage.sqlite',
    ], rootDir: root.path);

    expect(result.deletedIds, ['storage.sqlite']);
    expect(result.hasFailures, isFalse);
    expect(annotation.existsSync(), isFalse);
    expect(thumbnail.existsSync(), isTrue);
  });

  test(
    'enforces thumbnail cache limit by removing least recently used files',
    () async {
      final root = await Directory.systemTemp.createTemp(
        'void_storage_lru_thumbs_',
      );
      addTearDown(() => root.delete(recursive: true));

      final oldThumbnail = File(
        p.join(root.path, 'cache', 'media_hash', 'mark_thumbnails', 'old.png'),
      );
      await oldThumbnail.create(recursive: true);
      await oldThumbnail.writeAsBytes(List.filled(4, 1));
      final newThumbnail = File(
        p.join(root.path, 'cache', 'media_hash', 'mark_thumbnails', 'new.png'),
      );
      await newThumbnail.create(recursive: true);
      await newThumbnail.writeAsBytes(List.filled(4, 2));

      await oldThumbnail.setLastModified(DateTime(2024));
      await oldThumbnail.setLastAccessed(DateTime(2024));
      await newThumbnail.setLastModified(DateTime(2025));
      await newThumbnail.setLastAccessed(DateTime(2025));

      final result = await AppStorage.enforceMarkThumbnailLimit(
        maxBytes: 5,
        rootDir: root.path,
      );

      expect(result.deleteResult.deletedIds, [
        p.join('cache', 'media_hash', 'mark_thumbnails', 'old.png'),
      ]);
      expect(oldThumbnail.existsSync(), isFalse);
      expect(newThumbnail.existsSync(), isTrue);
      expect(result.snapshot.thumbnailBytes, 4);
    },
  );
}

Future<File> _createStorageDatabase(String rootPath) async {
  final file = File(p.join(rootPath, 'storage.sqlite'));
  final catalog = StorageCatalog(databasePath: file.path);
  final db = catalog.open();
  db.close();
  return file;
}
