import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/startup_options.dart';

void main() {
  test('parses loop range from direct CLI option', () {
    final options = StartupOptions.parse(['--loop-range=1.5s:4s']);

    expect(options.warnings, isEmpty);
    expect(options.loopRange?.startUs, 1500000);
    expect(options.loopRange?.endUs, 4000000);
  });

  test('parses loop range from deep link range', () {
    final options = StartupOptions.parse([
      '--deep-link',
      'voidplayer://v1/open?loopRange=1500ms:4s',
    ]);

    expect(options.warnings, isEmpty);
    expect(options.loopRange?.startUs, 1500000);
    expect(options.loopRange?.endUs, 4000000);
  });

  test('parses loop range from deep link start and end', () {
    final options = StartupOptions.parse([
      '--deep-link=voidplayer://v1/open?loopStart=1.5s&loopEnd=4s',
    ]);

    expect(options.warnings, isEmpty);
    expect(options.loopRange?.startUs, 1500000);
    expect(options.loopRange?.endUs, 4000000);
  });

  test('ignores unsupported deep link route', () {
    final options = StartupOptions.parse([
      '--deep-link',
      'voidplayer://v1/internal?loopRange=1s:2s',
    ]);

    expect(options.loopRange, isNull);
    expect(options.warnings, isNotEmpty);
  });
}
