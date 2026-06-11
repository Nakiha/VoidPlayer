import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:void_player/main_window/main_window_media.dart';

void main() {
  test('media filter skips duplicate sources in the same batch', () async {
    final result = await filterDuplicateMediaSources(const [
      'file:///tmp/a.mp4',
      'file:///tmp/a.mp4',
      'file:///tmp/b.mp4',
    ]);

    expect(result.uniqueSources, const [
      'file:///tmp/a.mp4',
      'file:///tmp/b.mp4',
    ]);
    expect(result.skippedCount, 1);
  });

  test('media filter skips sources already present in tracks', () async {
    final result = await filterDuplicateMediaSources(
      const ['file:///tmp/a.mp4', 'file:///tmp/c.mp4'],
      existingSources: const ['file:///tmp/a.mp4'],
    );

    expect(result.uniqueSources, const ['file:///tmp/c.mp4']);
    expect(result.skippedCount, 1);
  });

  test('media source identity normalizes URI paths', () async {
    final result = await filterDuplicateMediaSources(
      const ['https://example.test/media/../media/a.mp4'],
      existingSources: const ['https://example.test/media/a.mp4'],
    );

    expect(result.uniqueSources, isEmpty);
    expect(result.skippedCount, 1);
  });

  test('media source identity treats file URIs as local file paths', () async {
    final pathIdentity = await mediaSourceIdentity('/tmp/a.mp4');
    final uriIdentity = await mediaSourceIdentity('file:///tmp/a.mp4');

    expect(uriIdentity, pathIdentity);
  });

  test('media source identity falls back for missing local files', () async {
    final dir = await Directory.systemTemp.createTemp(
      'void_player_media_identity_test_',
    );
    addTearDown(() async {
      if (await dir.exists()) await dir.delete(recursive: true);
    });
    final missing = p.join(dir.path, 'missing.mp4');

    final identity = await mediaSourceIdentity(missing);

    expect(identity, p.normalize(File(missing).absolute.path));
  });
}
