import 'dart:io';

enum PathLauncherPlatform { windows, macOS, other }

typedef PathLauncherCommand =
    Future<void> Function(String executable, List<String> args);

abstract class PathLauncher {
  bool get isAvailable;

  Future<void> openFolder(String path);
  Future<void> locateFile(String path);
}

class LocalPathLauncher implements PathLauncher {
  const LocalPathLauncher()
    : this._(null, _runCheckedLauncher, _runDetachedLauncher);

  const LocalPathLauncher.testing({
    required PathLauncherPlatform platform,
    required PathLauncherCommand checkedLauncher,
    required PathLauncherCommand detachedLauncher,
  }) : this._(platform, checkedLauncher, detachedLauncher);

  const LocalPathLauncher._(
    this._platformOverride,
    this._checkedLauncher,
    this._detachedLauncher,
  );

  final PathLauncherPlatform? _platformOverride;
  final PathLauncherCommand _checkedLauncher;
  final PathLauncherCommand _detachedLauncher;

  PathLauncherPlatform get _platform {
    final override = _platformOverride;
    if (override != null) return override;
    if (Platform.isWindows) return PathLauncherPlatform.windows;
    if (Platform.isMacOS) return PathLauncherPlatform.macOS;
    return PathLauncherPlatform.other;
  }

  @override
  bool get isAvailable => true;

  @override
  Future<void> openFolder(String path) async {
    await Directory(path).create(recursive: true);
    if (_platform == PathLauncherPlatform.windows) {
      await _detachedLauncher('explorer.exe', [path]);
    } else if (_platform == PathLauncherPlatform.macOS) {
      await _checkedLauncher('open', [path]);
    } else {
      await _checkedLauncher('xdg-open', [path]);
    }
  }

  @override
  Future<void> locateFile(String path) async {
    final file = File(path);
    if (_platform == PathLauncherPlatform.windows) {
      if (await file.exists()) {
        await _detachedLauncher('explorer.exe', [
          '/select,',
          file.absolute.path,
        ]);
        return;
      }
      final parent = file.parent;
      if (await parent.exists()) {
        await _detachedLauncher('explorer.exe', [parent.absolute.path]);
      }
      return;
    }

    if (_platform == PathLauncherPlatform.macOS && await file.exists()) {
      await _checkedLauncher('open', ['-R', file.absolute.path]);
      return;
    }

    final target = await file.exists() ? file.absolute.path : file.parent.path;
    await openFolder(target);
  }
}

Future<void> _runDetachedLauncher(String executable, List<String> args) async {
  // Explorer delegates to an existing shell process and may report a non-zero
  // exit code after the folder has already opened. Process creation is the
  // meaningful success boundary for this fire-and-forget shell action.
  await Process.start(executable, args, mode: ProcessStartMode.detached);
}

Future<void> _runCheckedLauncher(String executable, List<String> args) async {
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
