import 'dart:io';
import 'dart:isolate';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:void_player/app_log.dart';

void main() {
  test('native log filename separates reused process ids by session', () {
    final config = LogConfig.defaultsFor(const []);

    expect(
      config.nativeLogFileName,
      matches(RegExp(r'^native_main_\d+_\d{4}-\d{2}-\d{2}_\d{6}_\d{3}\.log$')),
    );
  });

  test('file logger flushes queued records asynchronously', () async {
    final tempDir = await Directory.systemTemp.createTemp(
      'void_player_log_test_',
    );
    addTearDown(() async {
      await shutdownLogging();
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
      await shutdownLogging();
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

  test(
    'INFO filters component diagnostics but keeps named operations',
    () async {
      final tempDir = await Directory.systemTemp.createTemp(
        'void_player_log_level_test_',
      );
      addTearDown(() async {
        await shutdownLogging();
        if (await tempDir.exists()) {
          await tempDir.delete(recursive: true);
        }
      });

      await initLogging([
        '--log-level=flutter=INFO',
      ], logsDirOverride: tempDir.path);

      final component = appLogger('TestComponent');
      component.finer('per-event diagnostic');
      component.info('bounded operation completed');
      await flushLogFile();

      final file = await tempDir
          .list()
          .where((entity) => entity is File)
          .cast<File>()
          .single;
      final contents = await file.readAsString();
      expect(
        contents,
        contains('[TestComponent]: bounded operation completed'),
      );
      expect(contents, isNot(contains('per-event diagnostic')));
    },
  );

  test(
    'reinitialization and shutdown restore the Flutter error hook',
    () async {
      final tempDir = await Directory.systemTemp.createTemp(
        'void_player_log_hook_test_',
      );
      final original = FlutterError.onError;
      void sentinel(FlutterErrorDetails details) {}
      FlutterError.onError = sentinel;
      addTearDown(() async {
        await shutdownLogging();
        FlutterError.onError = original;
        if (await tempDir.exists()) {
          await tempDir.delete(recursive: true);
        }
      });

      await initLogging(const [], logsDirOverride: tempDir.path);
      expect(identical(FlutterError.onError, sentinel), isFalse);

      await initLogging(const [], logsDirOverride: tempDir.path);
      expect(identical(FlutterError.onError, sentinel), isFalse);

      await shutdownLogging();
      expect(identical(FlutterError.onError, sentinel), isTrue);
    },
  );

  test('concurrent log rotation tolerates files removed by peers', () async {
    final tempDir = await Directory.systemTemp.createTemp(
      'void_player_log_rotation_race_test_',
    );
    addTearDown(() async {
      await shutdownLogging();
      if (await tempDir.exists()) {
        await tempDir.delete(recursive: true);
      }
    });
    for (var index = 0; index < 48; index++) {
      final file = File(p.join(tempDir.path, 'void_player_stale_$index.log'));
      await file.writeAsString('stale');
      await file.setLastModified(
        DateTime(2020, 1, 1).add(Duration(seconds: index)),
      );
    }
    final logsDir = tempDir.path;

    await Future.wait(
      List.generate(
        8,
        (_) => Isolate.run(() async {
          await initLogging(const [], logsDirOverride: logsDir);
          await shutdownLogging();
        }),
      ),
    );

    expect(await tempDir.exists(), isTrue);
  });

  test('log retention includes native process logs', () async {
    final tempDir = await Directory.systemTemp.createTemp(
      'void_player_native_log_retention_test_',
    );
    addTearDown(() async {
      await shutdownLogging();
      if (await tempDir.exists()) {
        await tempDir.delete(recursive: true);
      }
    });
    for (var index = 0; index < 36; index++) {
      final file = File(
        p.join(tempDir.path, 'native_main_${index}_2020-01-01.log'),
      );
      await file.writeAsString('stale');
      await file.setLastModified(
        DateTime(2020, 1, 1).add(Duration(seconds: index)),
      );
    }

    await initLogging(const [], logsDirOverride: tempDir.path);
    await flushLogFile();

    final logFiles = await tempDir
        .list()
        .where((entity) => entity is File && entity.path.endsWith('.log'))
        .toList();
    // Retention runs before the current Dart process opens its own log.
    expect(logFiles.length, lessThanOrEqualTo(31));
    expect(
      logFiles
          .where((entity) => p.basename(entity.path).startsWith('native_'))
          .length,
      lessThan(36),
    );
  });
}
