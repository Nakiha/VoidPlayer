import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:void_player/storage/app_storage.dart';

void main() {
  test('scans annotation source data and thumbnail cache separately', () async {
    final root = await Directory.systemTemp.createTemp('void_storage_test_');
    addTearDown(() => root.delete(recursive: true));

    final annotation = File(p.join(root.path, 'annotations', 'local.vpmarks'));
    await annotation.create(recursive: true);
    await annotation.writeAsBytes([1, 2, 3, 4]);

    final thumbnail = File(p.join(root.path, 'marks', 'thumbnails', 'a.png'));
    await thumbnail.create(recursive: true);
    await thumbnail.writeAsBytes([5, 6, 7]);

    final snapshot = await AppStorage.scanMarkStorage(rootDir: root.path);

    expect(snapshot.annotationBytes, 4);
    expect(snapshot.annotationFileCount, 1);
    expect(snapshot.thumbnailBytes, 3);
    expect(snapshot.thumbnailFileCount, 1);
    expect(snapshot.totalBytes, 7);
  });

  test('clears thumbnails without deleting annotation source data', () async {
    final root = await Directory.systemTemp.createTemp('void_storage_clear_');
    addTearDown(() => root.delete(recursive: true));

    final annotation = File(p.join(root.path, 'annotations', 'local.vpmarks'));
    await annotation.create(recursive: true);
    await annotation.writeAsBytes([1, 2, 3, 4]);

    final thumbnail = File(p.join(root.path, 'marks', 'thumbnails', 'a.png'));
    await thumbnail.create(recursive: true);
    await thumbnail.writeAsBytes([5, 6, 7]);

    final result = await AppStorage.clearMarkThumbnails(rootDir: root.path);

    expect(result.deletedBytes, 3);
    expect(result.deletedFileCount, 1);
    expect(annotation.existsSync(), isTrue);
    expect(thumbnail.existsSync(), isFalse);

    final snapshot = await AppStorage.scanMarkStorage(rootDir: root.path);
    expect(snapshot.annotationBytes, 4);
    expect(snapshot.thumbnailBytes, 0);
  });

  test('deletes selected annotation data files only', () async {
    final root = await Directory.systemTemp.createTemp(
      'void_storage_delete_marks_',
    );
    addTearDown(() => root.delete(recursive: true));

    final annotation = File(p.join(root.path, 'annotations', 'local.vpmarks'));
    await annotation.create(recursive: true);
    await annotation.writeAsBytes([1, 2, 3, 4]);

    final thumbnail = File(p.join(root.path, 'marks', 'thumbnails', 'a.png'));
    await thumbnail.create(recursive: true);
    await thumbnail.writeAsBytes([5, 6, 7]);

    final result = await AppStorage.deleteMarkAnnotationFiles([
      'local.vpmarks',
    ], rootDir: root.path);

    expect(result.deletedIds, ['local.vpmarks']);
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
        p.join(root.path, 'marks', 'thumbnails', 'old.png'),
      );
      await oldThumbnail.create(recursive: true);
      await oldThumbnail.writeAsBytes(List.filled(4, 1));
      final newThumbnail = File(
        p.join(root.path, 'marks', 'thumbnails', 'new.png'),
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

      expect(result.deleteResult.deletedIds, ['old.png']);
      expect(oldThumbnail.existsSync(), isFalse);
      expect(newThumbnail.existsSync(), isTrue);
      expect(result.snapshot.thumbnailBytes, 4);
    },
  );
}
