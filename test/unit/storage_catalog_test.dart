import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
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
}
