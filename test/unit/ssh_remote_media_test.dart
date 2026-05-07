import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/remote/ssh_remote_media.dart';

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
    expect(remote.scpSource, "user@example.com:'/home/user/video file.mp4'");
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
}
