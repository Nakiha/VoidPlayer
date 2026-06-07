import 'dart:io';

import 'package:path/path.dart' as p;

class AppPathSet {
  final String exeDir;
  final String rootDir;
  final String configFile;
  final String locksDir;
  final String logsDir;
  final String storageDatabaseFile;
  final String analysisCacheDir;
  final String remoteCacheDir;
  final bool isPortable;

  const AppPathSet({
    required this.exeDir,
    required this.rootDir,
    required this.configFile,
    required this.locksDir,
    required this.logsDir,
    required this.storageDatabaseFile,
    required this.analysisCacheDir,
    required this.remoteCacheDir,
    required this.isPortable,
  });
}

class AppPaths {
  static const appDirName = 'VoidPlayer';
  static const portableMarkerDirName = 'cache';

  static AppPathSet get current => resolve();

  static AppPathSet resolve({
    String? executablePath,
    Map<String, String>? environment,
    String? operatingSystem,
    bool Function(String path)? directoryExists,
  }) {
    final resolvedExecutable = executablePath ?? Platform.resolvedExecutable;
    final pathContext = _pathContextFor(resolvedExecutable, operatingSystem);
    final exeDir = pathContext.dirname(resolvedExecutable);
    final exists = directoryExists ?? (path) => Directory(path).existsSync();
    final portableCacheDir = pathContext.join(exeDir, portableMarkerDirName);
    final isPortable = exists(portableCacheDir);
    final rootDir = isPortable
        ? exeDir
        : _defaultAppDataRoot(
            exeDir,
            environment,
            operatingSystem,
            pathContext,
          );
    return AppPathSet(
      exeDir: exeDir,
      rootDir: rootDir,
      configFile: pathContext.join(rootDir, 'config.json'),
      locksDir: pathContext.join(rootDir, 'locks'),
      logsDir: pathContext.join(rootDir, 'logs'),
      storageDatabaseFile: pathContext.join(rootDir, 'storage.sqlite'),
      analysisCacheDir: pathContext.join(rootDir, 'cache'),
      remoteCacheDir: pathContext.join(rootDir, 'remote_cache'),
      isPortable: isPortable,
    );
  }

  static p.Context _pathContextFor(
    String executablePath,
    String? operatingSystem,
  ) {
    if (operatingSystem == 'windows' ||
        executablePath.contains(r'\') ||
        RegExp(r'^[A-Za-z]:').hasMatch(executablePath)) {
      return p.Context(style: p.Style.windows);
    }
    return p.Context(style: p.Style.posix);
  }

  static String _defaultAppDataRoot(
    String exeDir,
    Map<String, String>? environment,
    String? operatingSystem,
    p.Context pathContext,
  ) {
    final env = environment ?? Platform.environment;
    final appData = _firstNonEmpty([env['APPDATA'], env['LOCALAPPDATA']]);
    if (appData != null) return pathContext.join(appData, appDirName);
    if ((operatingSystem ?? Platform.operatingSystem) == 'macos') {
      final home = _firstNonEmpty([env['HOME']]);
      if (home != null) {
        return pathContext.join(
          home,
          'Library',
          'Application Support',
          appDirName,
        );
      }
    }
    return pathContext.join(exeDir, appDirName);
  }

  static String? _firstNonEmpty(Iterable<String?> values) {
    for (final value in values) {
      if (value != null && value.trim().isNotEmpty) return value;
    }
    return null;
  }
}
