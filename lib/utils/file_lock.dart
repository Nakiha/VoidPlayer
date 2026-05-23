import 'dart:async';
import 'dart:io';

class FileLockHandle {
  final RandomAccessFile _file;
  final String _key;
  final _FileLockKind _kind;

  FileLockHandle._(this._file, this._key, this._kind);

  Future<void> release() async {
    try {
      await _file.unlock();
    } finally {
      FileLockService._unregisterInProcessLock(_key, _kind);
      await _file.close();
    }
  }

  void releaseSync() {
    try {
      _file.unlockSync();
    } finally {
      FileLockService._unregisterInProcessLock(_key, _kind);
      _file.closeSync();
    }
  }
}

enum _FileLockKind { shared, exclusive }

class _InProcessFileLockState {
  int shared = 0;
  int exclusive = 0;

  bool get isLocked => shared > 0 || exclusive > 0;
}

class FileLockService {
  const FileLockService._();

  static final Map<String, _InProcessFileLockState> _inProcessLocks = {};

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
    final key = _lockKey(lockPath);
    final file = await _openLockFile(lockPath);
    try {
      await file.lock(FileLock.blockingExclusive);
      _registerInProcessLock(key, _FileLockKind.exclusive);
      return FileLockHandle._(file, key, _FileLockKind.exclusive);
    } catch (_) {
      await file.close();
      rethrow;
    }
  }

  static Future<FileLockHandle> acquireShared(String lockPath) async {
    final key = _lockKey(lockPath);
    final file = await _openLockFile(lockPath);
    try {
      await file.lock(FileLock.blockingShared);
      _registerInProcessLock(key, _FileLockKind.shared);
      return FileLockHandle._(file, key, _FileLockKind.shared);
    } catch (_) {
      await file.close();
      rethrow;
    }
  }

  static Future<FileLockHandle?> tryAcquireExclusive(String lockPath) async {
    final key = _lockKey(lockPath);
    if (_hasInProcessLock(key)) return null;

    final file = await _openLockFile(lockPath);
    try {
      if (_hasInProcessLock(key)) {
        await file.close();
        return null;
      }
      await file.lock(FileLock.exclusive);
      if (_hasInProcessLock(key)) {
        await file.unlock();
        await file.close();
        return null;
      }
      _registerInProcessLock(key, _FileLockKind.exclusive);
      return FileLockHandle._(file, key, _FileLockKind.exclusive);
    } catch (_) {
      await file.close();
      return null;
    }
  }

  static FileLockHandle acquireSharedSync(String lockPath) {
    final key = _lockKey(lockPath);
    final file = _openLockFileSync(lockPath);
    try {
      file.lockSync(FileLock.blockingShared);
      _registerInProcessLock(key, _FileLockKind.shared);
      return FileLockHandle._(file, key, _FileLockKind.shared);
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
    final key = _lockKey(lockPath);
    if (_hasInProcessLock(key)) return null;

    final file = _openLockFileSync(lockPath);
    var locked = false;
    var registered = false;
    try {
      if (_hasInProcessLock(key)) {
        file.closeSync();
        return null;
      }
      file.lockSync(FileLock.exclusive);
      locked = true;
      if (_hasInProcessLock(key)) {
        file.unlockSync();
        file.closeSync();
        return null;
      }
      _registerInProcessLock(key, _FileLockKind.exclusive);
      registered = true;
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
      if (registered) _unregisterInProcessLock(key, _FileLockKind.exclusive);
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

  static String _lockKey(String lockPath) => File(lockPath).absolute.path;

  static bool _hasInProcessLock(String key) =>
      _inProcessLocks[key]?.isLocked ?? false;

  static void _registerInProcessLock(String key, _FileLockKind kind) {
    final state = _inProcessLocks.putIfAbsent(
      key,
      () => _InProcessFileLockState(),
    );
    switch (kind) {
      case _FileLockKind.shared:
        state.shared += 1;
        break;
      case _FileLockKind.exclusive:
        state.exclusive += 1;
        break;
    }
  }

  static void _unregisterInProcessLock(String key, _FileLockKind kind) {
    final state = _inProcessLocks[key];
    if (state == null) return;
    switch (kind) {
      case _FileLockKind.shared:
        state.shared -= 1;
        break;
      case _FileLockKind.exclusive:
        state.exclusive -= 1;
        break;
    }
    if (!state.isLocked) {
      _inProcessLocks.remove(key);
    }
  }
}
