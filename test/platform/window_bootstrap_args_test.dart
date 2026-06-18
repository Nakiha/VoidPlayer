import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/platform/window_bootstrap_args.dart';

void main() {
  test('matches exact and assigned CLI flags', () {
    expect(hasCliFlag(['--silent-ui-test'], '--silent-ui-test'), isTrue);
    expect(hasCliFlag(['--silent-ui-test=true'], '--silent-ui-test'), isTrue);
    expect(hasCliFlag(['--other'], '--silent-ui-test'), isFalse);
  });

  test('parses test window header from script file', () async {
    final dir = await Directory.systemTemp.createTemp('void-player-script-');
    addTearDown(() async {
      if (await dir.exists()) await dir.delete(recursive: true);
    });
    final script = File('${dir.path}/basic.csv');
    await script.writeAsString('@WINDOW, 960, 540\nOPEN,file.mp4\n');

    final window = parseTestWindowHeader(script.path);

    expect(window?.width, 960);
    expect(window?.height, 540);
  });

  test('ignores missing or malformed test window header', () async {
    final dir = await Directory.systemTemp.createTemp('void-player-script-');
    addTearDown(() async {
      if (await dir.exists()) await dir.delete(recursive: true);
    });
    final script = File('${dir.path}/bad.csv');
    await script.writeAsString('@WINDOW, wide, tall\n');

    expect(parseTestWindowHeader('${dir.path}/missing.csv'), isNull);
    expect(parseTestWindowHeader(script.path), isNull);
  });
}
