import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/app_log.dart';

void main() {
  test('file logger flushes queued records asynchronously', () async {
    final tempDir = await Directory.systemTemp.createTemp(
      'void_player_log_test_',
    );
    addTearDown(() async {
      await flushLogFile();
      if (await tempDir.exists()) {
        await tempDir.delete(recursive: true);
      }
    });

    await initLogging([
      '--log-level=flutter=INFO',
    ], logsDirOverride: tempDir.path);

    log.info('queued async file log record');
    await flushLogFile();

    final files = await tempDir
        .list()
        .where((entity) => entity is File)
        .cast<File>()
        .toList();
    expect(files, hasLength(1));

    final contents = await files.single.readAsString();
    expect(contents, contains('queued async file log record'));
  });

  test('file logger drains multiple queued records in order', () async {
    final tempDir = await Directory.systemTemp.createTemp(
      'void_player_log_batch_test_',
    );
    addTearDown(() async {
      await flushLogFile();
      if (await tempDir.exists()) {
        await tempDir.delete(recursive: true);
      }
    });

    await initLogging([
      '--log-level=flutter=INFO',
    ], logsDirOverride: tempDir.path);

    for (var i = 0; i < 3; i++) {
      log.info('queued async file log record $i');
    }
    await flushLogFile();

    final files = await tempDir
        .list()
        .where((entity) => entity is File)
        .cast<File>()
        .toList();
    expect(files, hasLength(1));

    final contents = await files.single.readAsString();
    expect(
      contents.indexOf('queued async file log record 0'),
      lessThan(contents.indexOf('queued async file log record 2')),
    );
  });
}
