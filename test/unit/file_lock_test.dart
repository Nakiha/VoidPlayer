import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/utils/file_lock.dart';

void main() {
  test('exclusive lock is not acquired while shared lock is held', () async {
    final dir = await Directory.systemTemp.createTemp('void_player_lock_test_');
    addTearDown(() async {
      if (await dir.exists()) await dir.delete(recursive: true);
    });
    final lockPath = '${dir.path}${Platform.pathSeparator}sample.lock';
    final shared = FileLockService.acquireSharedSync(lockPath);
    addTearDown(shared.releaseSync);

    final result = await FileLockService.tryExclusive(lockPath, () => true);

    expect(result, isNull);
  });
}
