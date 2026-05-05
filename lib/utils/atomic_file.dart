import 'dart:convert';
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

typedef _MoveFileExWNative =
    Int32 Function(
      Pointer<Utf16> lpExistingFileName,
      Pointer<Utf16> lpNewFileName,
      Uint32 dwFlags,
    );
typedef _MoveFileExWDart =
    int Function(
      Pointer<Utf16> lpExistingFileName,
      Pointer<Utf16> lpNewFileName,
      int dwFlags,
    );

const int _movefileReplaceExisting = 0x00000001;
const int _movefileWriteThrough = 0x00000008;

class AtomicFileWriter {
  AtomicFileWriter._();

  static Future<void> writeString(
    File target,
    String contents, {
    Encoding encoding = utf8,
  }) async {
    await target.parent.create(recursive: true);
    final tmp = File(_temporaryPath(target.path));
    var committed = false;
    try {
      await tmp.writeAsString(contents, encoding: encoding, flush: true);
      await _replace(tmp, target);
      committed = true;
    } finally {
      if (!committed) await _deleteTemporary(tmp);
    }
  }

  static void writeStringSync(
    File target,
    String contents, {
    Encoding encoding = utf8,
  }) {
    target.parent.createSync(recursive: true);
    final tmp = File(_temporaryPath(target.path));
    var committed = false;
    try {
      tmp.writeAsStringSync(contents, encoding: encoding, flush: true);
      _replaceSync(tmp, target);
      committed = true;
    } finally {
      if (!committed) _deleteTemporarySync(tmp);
    }
  }

  static String _temporaryPath(String targetPath) =>
      '$targetPath.$pid.${DateTime.now().microsecondsSinceEpoch}.tmp';

  static Future<void> _replace(File tmp, File target) async {
    if (Platform.isWindows) {
      _replaceWindows(tmp.path, target.path);
      return;
    }
    try {
      await tmp.rename(target.path);
    } on FileSystemException {
      if (await target.exists()) await target.delete();
      await tmp.rename(target.path);
    }
  }

  static void _replaceSync(File tmp, File target) {
    if (Platform.isWindows) {
      _replaceWindows(tmp.path, target.path);
      return;
    }
    try {
      tmp.renameSync(target.path);
    } on FileSystemException {
      if (target.existsSync()) target.deleteSync();
      tmp.renameSync(target.path);
    }
  }

  static void _replaceWindows(String sourcePath, String targetPath) {
    final source = sourcePath.toNativeUtf16();
    final target = targetPath.toNativeUtf16();
    try {
      final ok = _WindowsFileMove.moveFileExW(
        source,
        target,
        _movefileReplaceExisting | _movefileWriteThrough,
      );
      if (ok == 0) {
        throw FileSystemException(
          'MoveFileExW failed while replacing file',
          targetPath,
        );
      }
    } finally {
      calloc.free(source);
      calloc.free(target);
    }
  }

  static Future<void> _deleteTemporary(File tmp) async {
    try {
      if (await tmp.exists()) await tmp.delete();
    } catch (_) {
      // Best-effort cleanup after a failed commit.
    }
  }

  static void _deleteTemporarySync(File tmp) {
    try {
      if (tmp.existsSync()) tmp.deleteSync();
    } catch (_) {
      // Best-effort cleanup after a failed commit.
    }
  }
}

class _WindowsFileMove {
  _WindowsFileMove._();

  static final _MoveFileExWDart moveFileExW = DynamicLibrary.open(
    'kernel32.dll',
  ).lookupFunction<_MoveFileExWNative, _MoveFileExWDart>('MoveFileExW');
}
