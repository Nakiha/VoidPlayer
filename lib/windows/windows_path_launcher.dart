import 'dart:io';

import '../platform/path_launcher.dart';

class WindowsPathLauncher implements PathLauncher {
  const WindowsPathLauncher();

  @override
  Future<void> openFolder(String path) async {
    await Directory(path).create(recursive: true);
    await Process.start('explorer.exe', [path]);
  }

  @override
  Future<void> locateFile(String path) async {
    final file = File(path);
    if (await file.exists()) {
      await Process.start('explorer.exe', ['/select,', file.absolute.path]);
      return;
    }
    final parent = file.parent;
    if (await parent.exists()) {
      await Process.start('explorer.exe', [parent.absolute.path]);
    }
  }
}
