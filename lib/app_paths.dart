import 'dart:io';

import 'package:path/path.dart' as p;

class AppPathSet {
  final String exeDir;
  final String rootDir;
  final String configFile;
  final String locksDir;
  final String logsDir;
  final String analysisCacheDir;
  final String remoteCacheDir;
  final bool isPortable;

  const AppPathSet({
    required this.exeDir,
    required this.rootDir,
    required this.configFile,
    required this.locksDir,
    required this.logsDir,
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
    bool Function(String path)? directoryExists,
  }) {
    final exeDir = p.dirname(executablePath ?? Platform.resolvedExecutable);
    final exists = directoryExists ?? (path) => Directory(path).existsSync();
    final portableCacheDir = p.join(exeDir, portableMarkerDirName);
    final isPortable = exists(portableCacheDir);
    final rootDir = isPortable
        ? exeDir
        : _defaultAppDataRoot(exeDir, environment);
    return AppPathSet(
      exeDir: exeDir,
      rootDir: rootDir,
      configFile: p.join(rootDir, 'config.json'),
      locksDir: p.join(rootDir, 'locks'),
      logsDir: p.join(rootDir, 'logs'),
      analysisCacheDir: p.join(rootDir, 'cache'),
      remoteCacheDir: p.join(rootDir, 'remote_cache'),
      isPortable: isPortable,
    );
  }

  static String _defaultAppDataRoot(
    String exeDir,
    Map<String, String>? environment,
  ) {
    final env = environment ?? Platform.environment;
    final appData = _firstNonEmpty([env['APPDATA'], env['LOCALAPPDATA']]);
    if (appData != null) return p.join(appData, appDirName);
    return p.join(exeDir, appDirName);
  }

  static String? _firstNonEmpty(Iterable<String?> values) {
    for (final value in values) {
      if (value != null && value.trim().isNotEmpty) return value;
    }
    return null;
  }
}
