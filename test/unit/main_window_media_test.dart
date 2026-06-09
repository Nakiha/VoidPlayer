import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/windows/main/main_window_media.dart';

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
}
