import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/platform/path_launcher.dart';

void main() {
  late Directory tempDirectory;
  late List<({String executable, List<String> args})> detachedCalls;
  late List<({String executable, List<String> args})> checkedCalls;

  setUp(() async {
    tempDirectory = await Directory.systemTemp.createTemp(
      'void-player-path-launcher-',
    );
    detachedCalls = [];
    checkedCalls = [];
  });

  tearDown(() async {
    await tempDirectory.delete(recursive: true);
  });

  LocalPathLauncher windowsLauncher() {
    return LocalPathLauncher.testing(
      platform: PathLauncherPlatform.windows,
      checkedLauncher: (executable, args) async {
        checkedCalls.add((executable: executable, args: List.of(args)));
      },
      detachedLauncher: (executable, args) async {
        detachedCalls.add((executable: executable, args: List.of(args)));
      },
    );
  }

  test('Windows locate uses detached Explorer for an existing file', () async {
    final file = File('${tempDirectory.path}${Platform.pathSeparator}clip.mp4');
    await file.writeAsBytes(const [0]);

    await windowsLauncher().locateFile(file.path);

    expect(checkedCalls, isEmpty);
    expect(detachedCalls, hasLength(1));
    expect(detachedCalls.single.executable, 'explorer.exe');
    expect(detachedCalls.single.args, ['/select,', file.absolute.path]);
  });

  test('Windows missing file opens its parent without checking exit', () async {
    final missing = File(
      '${tempDirectory.path}${Platform.pathSeparator}missing.mp4',
    );

    await windowsLauncher().locateFile(missing.path);

    expect(checkedCalls, isEmpty);
    expect(detachedCalls, hasLength(1));
    expect(detachedCalls.single.executable, 'explorer.exe');
    expect(detachedCalls.single.args, [tempDirectory.absolute.path]);
  });

  test('Windows Explorer spawn failures still propagate', () async {
    final file = File('${tempDirectory.path}${Platform.pathSeparator}clip.mp4');
    await file.writeAsBytes(const [0]);
    final launcher = LocalPathLauncher.testing(
      platform: PathLauncherPlatform.windows,
      checkedLauncher: (executable, args) async {},
      detachedLauncher: (executable, args) async {
        throw ProcessException(executable, args, 'spawn failed');
      },
    );

    await expectLater(
      launcher.locateFile(file.path),
      throwsA(isA<ProcessException>()),
    );
  });

  test('non-Windows launchers retain checked exit semantics', () async {
    final launcher = LocalPathLauncher.testing(
      platform: PathLauncherPlatform.macOS,
      checkedLauncher: (executable, args) async {
        checkedCalls.add((executable: executable, args: List.of(args)));
      },
      detachedLauncher: (executable, args) async {
        detachedCalls.add((executable: executable, args: List.of(args)));
      },
    );

    await launcher.openFolder(tempDirectory.path);

    expect(detachedCalls, isEmpty);
    expect(checkedCalls, hasLength(1));
    expect(checkedCalls.single.executable, 'open');
    expect(checkedCalls.single.args, [tempDirectory.path]);
  });
}
