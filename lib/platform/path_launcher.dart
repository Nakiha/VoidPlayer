import 'dart:io';

abstract class PathLauncher {
  bool get isAvailable;

  Future<void> openFolder(String path);
  Future<void> locateFile(String path);
}

class LocalPathLauncher implements PathLauncher {
  const LocalPathLauncher();

  @override
  bool get isAvailable => true;

  @override
  Future<void> openFolder(String path) async {
    await Directory(path).create(recursive: true);
    if (Platform.isWindows) {
      await _runLauncher('explorer.exe', [path]);
    } else if (Platform.isMacOS) {
      await _runLauncher('open', [path]);
    } else {
      await _runLauncher('xdg-open', [path]);
    }
  }

  @override
  Future<void> locateFile(String path) async {
    final file = File(path);
    if (Platform.isWindows) {
      if (await file.exists()) {
        await _runLauncher('explorer.exe', ['/select,', file.absolute.path]);
        return;
      }
      final parent = file.parent;
      if (await parent.exists()) {
        await _runLauncher('explorer.exe', [parent.absolute.path]);
      }
      return;
    }

    if (Platform.isMacOS && await file.exists()) {
      await _runLauncher('open', ['-R', file.absolute.path]);
      return;
    }

    final target = await file.exists() ? file.absolute.path : file.parent.path;
    await openFolder(target);
  }

  static Future<void> _runLauncher(String executable, List<String> args) async {
    final result = await Process.run(executable, args);
    if (result.exitCode != 0) {
      throw ProcessException(
        executable,
        args,
        result.stderr.toString(),
        result.exitCode,
      );
    }
  }
}

class UnsupportedPathLauncher implements PathLauncher {
  const UnsupportedPathLauncher({
    this.message = 'Path launching is not available on this platform yet.',
  });

  final String message;

  @override
  bool get isAvailable => false;

  @override
  Future<void> openFolder(String path) {
    throw UnsupportedError(message);
  }

  @override
  Future<void> locateFile(String path) {
    throw UnsupportedError(message);
  }
}
