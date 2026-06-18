import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/utils/atomic_file.dart';

void main() {
  late Directory dir;

  setUp(() async {
    dir = await Directory.systemTemp.createTemp(
      'void_player_atomic_file_test_',
    );
  });

  tearDown(() async {
    if (await dir.exists()) await dir.delete(recursive: true);
  });

  test('writeString atomically replaces an existing file', () async {
    final file = File('${dir.path}/state.json');
    await file.writeAsString('old');

    await AtomicFileWriter.writeString(file, 'new');

    expect(await file.readAsString(), 'new');
    expect(_temporaryFilesFor(file), isEmpty);
  });

  test('writeStringSync atomically replaces an existing file', () {
    final file = File('${dir.path}/index.json');
    file.writeAsStringSync('old');

    AtomicFileWriter.writeStringSync(file, 'new');

    expect(file.readAsStringSync(), 'new');
    expect(_temporaryFilesFor(file), isEmpty);
  });
}

List<FileSystemEntity> _temporaryFilesFor(File target) {
  final prefix = '${target.path}.';
  return target.parent
      .listSync(followLinks: false)
      .where((entity) => entity.path.startsWith(prefix))
      .toList();
}
