import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/file_hash.dart';

void main() {
  test('computeFileSha256 matches a known digest', () async {
    final dir = await Directory.systemTemp.createTemp('void_player_hash_');
    addTearDown(() async {
      if (await dir.exists()) await dir.delete(recursive: true);
    });
    final file = File('${dir.path}/abc.txt');
    await file.writeAsString('abc');

    expect(
      await computeFileSha256(file.path),
      'ba7816bf8f01cfea414140de5dae2223'
      'b00361a396177a9cb410ff61f20015ad',
    );
  });

  test('computeFileSha256 includes bytes after the first megabyte', () async {
    final dir = await Directory.systemTemp.createTemp('void_player_hash_');
    addTearDown(() async {
      if (await dir.exists()) await dir.delete(recursive: true);
    });
    final prefix = List<int>.filled(1024 * 1024, 7);
    final first = File('${dir.path}/first.bin');
    final second = File('${dir.path}/second.bin');
    await first.writeAsBytes([...prefix, 1]);
    await second.writeAsBytes([...prefix, 2]);

    expect(
      await computeFileSha256(first.path),
      isNot(await computeFileSha256(second.path)),
    );
  });
}
