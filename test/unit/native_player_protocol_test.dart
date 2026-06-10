import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/video_renderer_controller.dart';

void main() {
  test(
    'create player result rejects missing texture id with protocol error',
    () {
      expect(
        () => CreatePlayerResult.fromMap(const {'tracks': <Object>[]}),
        throwsA(isA<NativeProtocolException>()),
      );
    },
  );

  test('track info rejects payload type drift with protocol error', () {
    expect(
      () => TrackInfo.fromMap(const {
        'fileId': '1',
        'slot': 0,
        'path': '/tmp/video.mp4',
        'width': 320,
        'height': 180,
      }),
      throwsA(
        isA<NativeProtocolException>().having(
          (error) => error.reason,
          'reason',
          contains('fileId'),
        ),
      ),
    );
  });

  test('viewport capture rejects non-numeric region metrics', () {
    expect(
      () => ViewportCapture.fromMap(const {
        'hash': 'abc',
        'width': 320,
        'height': 180,
        'regionAvgLuma': {'left': 'bright'},
      }),
      throwsA(isA<NativeProtocolException>()),
    );
  });

  test('playback snapshot parses presented frames by file id', () {
    final snapshot = PlaybackSnapshot.fromMap(const {
      'currentPtsUs': 123,
      'durationUs': 456,
      'isPlaying': true,
      'presentedFrames': [
        {'fileId': 7, 'ptsUs': 120, 'dtsUs': 100},
      ],
    });

    expect(snapshot.currentPtsUs, 123);
    expect(snapshot.durationUs, 456);
    expect(snapshot.isPlaying, isTrue);
    expect(snapshot.presentedFrames[7]?.ptsUs, 120);
  });
}
