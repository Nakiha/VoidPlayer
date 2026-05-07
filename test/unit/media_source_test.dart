import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/utils/media_source.dart';

void main() {
  test('recognizes only http and https media urls', () {
    expect(isHttpMediaUrl('http://127.0.0.1:8000/a.mp4'), isTrue);
    expect(isHttpMediaUrl('https://example.com/video.webm'), isTrue);
    expect(isHttpMediaUrl('file:///C:/video.mp4'), isFalse);
    expect(isHttpMediaUrl('not a url'), isFalse);
  });

  test('extracts dropped urls and file paths', () {
    expect(
      mediaSourcesFromDroppedValues([
        'watch this: https://example.com/video.mp4',
        'file:///C:/Videos/local.mp4',
        r'D:\media\clip.ts',
      ]),
      [
        'https://example.com/video.mp4',
        r'C:\Videos\local.mp4',
        r'D:\media\clip.ts',
      ],
    );
  });
}
