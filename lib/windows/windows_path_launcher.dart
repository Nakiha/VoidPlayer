import 'dart:io';

import '../platform/path_launcher.dart';

class WindowsPathLauncher implements PathLauncher {
  const WindowsPathLauncher();

  @override
  Future<void> openFolder(String path) async {
    await Directory(path).create(recursive: true);
    await Process.start('explorer.exe', [path]);
  }
}
