import 'dart:async';
import 'dart:io';

class FileLockHandle {
  final RandomAccessFile _file;

  FileLockHandle._(this._file);

  Future<void> release() async {
    try {
      await _file.unlock();
    } finally {
      await _file.close();
    }
  }

  void releaseSync() {
    try {
      _file.unlockSync();
    } finally {
      _file.closeSync();
    }
  }
}

class FileLockService {
  const FileLockService._();

  static Future<T> withExclusive<T>(
    String lockPath,
    FutureOr<T> Function() action,
  ) async {
    final handle = await acquireExclusive(lockPath);
    try {
      return await action();
    } finally {
      await handle.release();
    }
  }

  static Future<T> withShared<T>(
    String lockPath,
    FutureOr<T> Function() action,
  ) async {
    final handle = await acquireShared(lockPath);
    try {
      return await action();
    } finally {
      await handle.release();
    }
  }

  static Future<T?> tryExclusive<T>(
    String lockPath,
    FutureOr<T> Function() action,
  ) async {
    final handle = await tryAcquireExclusive(lockPath);
    if (handle == null) return null;
    try {
      return await action();
    } finally {
      await handle.release();
    }
  }

  static Future<FileLockHandle> acquireExclusive(String lockPath) async {
    final file = await _openLockFile(lockPath);
    try {
      await file.lock(FileLock.blockingExclusive);
      return FileLockHandle._(file);
    } catch (_) {
      await file.close();
      rethrow;
    }
  }

  static Future<FileLockHandle> acquireShared(String lockPath) async {
    final file = await _openLockFile(lockPath);
    try {
      await file.lock(FileLock.blockingShared);
      return FileLockHandle._(file);
    } catch (_) {
      await file.close();
      rethrow;
    }
  }

  static Future<FileLockHandle?> tryAcquireExclusive(String lockPath) async {
    final file = await _openLockFile(lockPath);
    try {
      await file.lock(FileLock.exclusive);
      return FileLockHandle._(file);
    } catch (_) {
      await file.close();
      return null;
    }
  }

  static FileLockHandle acquireSharedSync(String lockPath) {
    final file = _openLockFileSync(lockPath);
    try {
      file.lockSync(FileLock.blockingShared);
      return FileLockHandle._(file);
    } catch (_) {
      file.closeSync();
      rethrow;
    }
  }

  static T withExclusiveSync<T>(String lockPath, T Function() action) {
    final file = _openLockFileSync(lockPath);
    try {
      file.lockSync(FileLock.blockingExclusive);
      return action();
    } finally {
      file.unlockSync();
      file.closeSync();
    }
  }

  static T withSharedSync<T>(String lockPath, T Function() action) {
    final file = _openLockFileSync(lockPath);
    try {
      file.lockSync(FileLock.blockingShared);
      return action();
    } finally {
      file.unlockSync();
      file.closeSync();
    }
  }

  static T? tryExclusiveSync<T>(String lockPath, T Function() action) {
    final file = _openLockFileSync(lockPath);
    var locked = false;
    try {
      file.lockSync(FileLock.exclusive);
      locked = true;
    } catch (_) {
      file.closeSync();
      return null;
    }

    try {
      return action();
    } catch (_) {
      rethrow;
    } finally {
      try {
        if (locked) file.unlockSync();
      } catch (_) {}
      file.closeSync();
    }
  }

  static Future<RandomAccessFile> _openLockFile(String lockPath) async {
    final file = File(lockPath);
    await file.parent.create(recursive: true);
    return file.open(mode: FileMode.append);
  }

  static RandomAccessFile _openLockFileSync(String lockPath) {
    final file = File(lockPath);
    file.parent.createSync(recursive: true);
    return file.openSync(mode: FileMode.append);
  }
}
