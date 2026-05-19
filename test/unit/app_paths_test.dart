import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/app_paths.dart';

void main() {
  test('uses AppData root by default', () {
    final paths = AppPaths.resolve(
      executablePath: r'C:\Program Files\VoidPlayer\void_player.exe',
      environment: const {'APPDATA': r'C:\Users\me\AppData\Roaming'},
      directoryExists: (_) => false,
    );

    expect(paths.isPortable, isFalse);
    expect(paths.rootDir, r'C:\Users\me\AppData\Roaming\VoidPlayer');
    expect(
      paths.configFile,
      r'C:\Users\me\AppData\Roaming\VoidPlayer\config.json',
    );
    expect(paths.locksDir, r'C:\Users\me\AppData\Roaming\VoidPlayer\locks');
    expect(paths.logsDir, r'C:\Users\me\AppData\Roaming\VoidPlayer\logs');
    expect(
      paths.analysisCacheDir,
      r'C:\Users\me\AppData\Roaming\VoidPlayer\cache',
    );
    expect(
      paths.remoteCacheDir,
      r'C:\Users\me\AppData\Roaming\VoidPlayer\remote_cache',
    );
  });

  test('uses executable directory when cache marker exists beside exe', () {
    final paths = AppPaths.resolve(
      executablePath: r'D:\Portable\VoidPlayer\void_player.exe',
      environment: const {'APPDATA': r'C:\Users\me\AppData\Roaming'},
      directoryExists: (path) => path == r'D:\Portable\VoidPlayer\cache',
    );

    expect(paths.isPortable, isTrue);
    expect(paths.rootDir, r'D:\Portable\VoidPlayer');
    expect(paths.configFile, r'D:\Portable\VoidPlayer\config.json');
    expect(paths.locksDir, r'D:\Portable\VoidPlayer\locks');
    expect(paths.logsDir, r'D:\Portable\VoidPlayer\logs');
    expect(paths.analysisCacheDir, r'D:\Portable\VoidPlayer\cache');
    expect(paths.remoteCacheDir, r'D:\Portable\VoidPlayer\remote_cache');
  });

  test('falls back to LocalAppData when AppData is unavailable', () {
    final paths = AppPaths.resolve(
      executablePath: r'C:\Program Files\VoidPlayer\void_player.exe',
      environment: const {'LOCALAPPDATA': r'C:\Users\me\AppData\Local'},
      directoryExists: (_) => false,
    );

    expect(paths.rootDir, r'C:\Users\me\AppData\Local\VoidPlayer');
  });

  test('uses Application Support root on macOS', () {
    final paths = AppPaths.resolve(
      executablePath: '/Applications/VoidPlayer.app/Contents/MacOS/VoidPlayer',
      environment: const {'HOME': '/Users/me'},
      operatingSystem: 'macos',
      directoryExists: (_) => false,
    );

    expect(paths.isPortable, isFalse);
    expect(paths.rootDir, '/Users/me/Library/Application Support/VoidPlayer');
    expect(
      paths.logsDir,
      '/Users/me/Library/Application Support/VoidPlayer/logs',
    );
  });

  test('falls back to executable directory when AppData is unavailable', () {
    final paths = AppPaths.resolve(
      executablePath: r'D:\Tools\VoidPlayer\void_player.exe',
      environment: const {},
      directoryExists: (_) => false,
    );

    expect(paths.rootDir, r'D:\Tools\VoidPlayer\VoidPlayer');
  });
}
