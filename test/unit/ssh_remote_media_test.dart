import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/remote/ssh_remote_media.dart';
import 'package:void_player/widgets/open_ssh_remote_file_dialog.dart';

void main() {
  test('parses scp-style remote paths', () {
    final remote = SshRemoteFile.parse('user@example.com:/videos/clip.mp4');

    expect(remote.host, 'user@example.com');
    expect(remote.path, '/videos/clip.mp4');
    expect(remote.port, isNull);
    expect(remote.display, 'user@example.com:/videos/clip.mp4');
    expect(remote.cacheFileName, endsWith('-clip.mp4'));
  });

  test('parses ssh URI remote paths with port', () {
    final remote = SshRemoteFile.parse(
      'ssh://user@example.com:2222/home/user/video file.mp4',
    );

    expect(remote.host, 'user@example.com');
    expect(remote.path, '/home/user/video file.mp4');
    expect(remote.port, 2222);
    expect(remote.scpArgs('out.mp4'), contains('-P'));
    expect(remote.scpSource, 'user@example.com:/home/user/video file.mp4');
    expect(
      remote.legacyScpSource,
      "user@example.com:'/home/user/video file.mp4'",
    );
    expect(
      remote.sftpUrl,
      'sftp://user@example.com:2222/home/user/video file.mp4',
    );
  });

  test('rejects unsupported remote paths', () {
    expect(
      () => SshRemoteFile.parse('https://example.com/video.mp4'),
      throwsA(isA<SshRemoteMediaException>()),
    );
  });

  test('builds search command that expands home directory remotely', () {
    const service = SshRemoteMediaService();

    final command = service.buildFindCommand(
      directory: '~/',
      pattern: '*.mp4',
      limit: 100,
    );

    expect(command, r"find $HOME'/' -type f -iname '*.mp4' | head -n 100");
    expect(command, isNot(contains("'~/'")));
  });

  test('converts remote search results to playable sftp URLs', () {
    const result = SshRemoteSearchResult(
      host: 'zhuhongwei@192.168.1.103',
      path: '/Users/zhuhongwei/Movies/h266_10s_1920x1080_副本.mp4',
    );

    expect(result.fileName, 'h266_10s_1920x1080_副本.mp4');
    expect(
      result.sftpUrl,
      'sftp://zhuhongwei@192.168.1.103'
      '/Users/zhuhongwei/Movies/h266_10s_1920x1080_副本.mp4',
    );
  });

  test('playable input preserves existing sftp URLs', () {
    const service = SshRemoteMediaService();

    expect(
      service.playableInput(
        'sftp://zhuhongwei@192.168.1.103/Users/zhuhongwei/video.mp4',
      ),
      'sftp://zhuhongwei@192.168.1.103/Users/zhuhongwei/video.mp4',
    );
  });

  testWidgets('SSH search dialog reports validation errors', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: const Scaffold(body: OpenSshRemoteFileDialog()),
      ),
    );

    await tester.tap(find.widgetWithText(FilledButton, 'Search'));
    await tester.pumpAndSettle();

    expect(find.text('Host and directory are required.'), findsOneWidget);
    expect(find.widgetWithText(FilledButton, 'Search'), findsOneWidget);
  });
}
