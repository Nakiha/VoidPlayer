import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/video_renderer_controller.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const channel = MethodChannel('video_renderer');

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  test('playback commands no-op before player creation', () async {
    final calls = <String>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          calls.add(call.method);
          return null;
        });
    final controller = NativePlayerController();

    await controller.play();
    await controller.pause();
    await controller.seek(1000);
    await controller.stepForward();
    await controller.stepBackward();

    expect(calls, isEmpty);
    expect(await controller.currentPts(), 0);
    expect(await controller.currentPresentedFrame(1), isNull);
    expect(await controller.duration(), 0);
    expect(await controller.isPlaying(), isFalse);
    final snapshot = await controller.getPlaybackSnapshot();
    expect(snapshot.currentPtsUs, 0);
    expect(snapshot.durationUs, 0);
    expect(snapshot.isPlaying, isFalse);
  });

  test('tolerant no-player commands report diagnostics', () async {
    final diagnostics = <String>[];
    final controller = NativePlayerController(
      onNoopCommand: (method, reason) => diagnostics.add('$method:$reason'),
    );

    await controller.play();
    await controller.currentPts();
    await controller.getDiagnostics();

    expect(diagnostics, [
      'play:player not created',
      'currentPts:player not created',
      'getDiagnostics:player not created',
    ]);
  });

  test(
    'strict command order rejects tolerant no-op commands before creation',
    () {
      final controller = NativePlayerController(strictCommandOrder: true);

      expect(() => controller.play(), throwsStateError);
      expect(() => controller.seek(1000), throwsStateError);
      expect(() => controller.currentPts(), throwsStateError);
      expect(() => controller.getPlaybackSnapshot(), throwsStateError);
      expect(
        () => controller.applyLayout(const LayoutState()),
        throwsStateError,
      );
      expect(() => controller.getDiagnostics(), throwsStateError);
    },
  );

  test('structural commands require a created player', () async {
    final controller = NativePlayerController();

    expect(() => controller.getLayout(), throwsStateError);
    expect(() => controller.addTrack('next.mp4'), throwsStateError);
    expect(() => controller.removeTrack(1), throwsStateError);
    expect(
      () => controller.setTrackOffset(fileId: 1, offsetUs: 1000),
      throwsStateError,
    );
  });

  test('getLayout is allowed after player creation', () async {
    final calls = <String>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          calls.add(call.method);
          if (call.method == 'createPlayer') {
            return {
              'playerId': 7,
              'textureId': 7,
              'tracks': <Map<String, Object>>[
                {
                  'fileId': 11,
                  'slot': 0,
                  'path': 'a.mp4',
                  'width': 1920,
                  'height': 1080,
                  'durationUs': 1000,
                },
              ],
            };
          }
          if (call.method == 'getLayout') {
            return {
              'mode': 0,
              'order': <int>[11, -1, -1, -1],
            };
          }
          if (call.method == 'destroyPlayer') return null;
          return null;
        });
    final controller = NativePlayerController();

    await controller.createPlayer(const ['a.mp4']);
    final layout = await controller.getLayout();
    await controller.dispose();

    expect(layout.order, const [11, -1, -1, -1]);
    expect(calls, containsAllInOrder(['createPlayer', 'getLayout']));
  });

  test(
    'playback getter null payloads are protocol errors after creation',
    () async {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (call) async {
            if (call.method == 'createPlayer') {
              return {
                'playerId': 7,
                'textureId': 7,
                'tracks': <Map<String, Object>>[
                  {
                    'fileId': 11,
                    'slot': 0,
                    'path': 'a.mp4',
                    'width': 1920,
                    'height': 1080,
                  },
                ],
              };
            }
            if (call.method == 'destroyPlayer') return null;
            if (call.method == 'getPlaybackSnapshot') {
              return {
                'currentPtsUs': 1234,
                'durationUs': 1000,
                'isPlaying': true,
                'presentedFrames': <Map<String, Object>>[
                  {'fileId': 11, 'ptsUs': 1234, 'dtsUs': 1200},
                ],
              };
            }
            return null;
          });
      final controller = NativePlayerController();

      await controller.createPlayer(const ['a.mp4']);
      addTearDown(controller.dispose);

      await expectLater(
        controller.currentPts(),
        throwsA(isA<NativeProtocolException>()),
      );
      await expectLater(
        controller.duration(),
        throwsA(isA<NativeProtocolException>()),
      );
      await expectLater(
        controller.isPlaying(),
        throwsA(isA<NativeProtocolException>()),
      );
      final snapshot = await controller.getPlaybackSnapshot(
        includePresentedFrames: true,
      );
      expect(snapshot.currentPtsUs, 1234);
      expect(snapshot.isPlaying, isTrue);
      expect(snapshot.presentedFrames[11]?.dtsUs, 1200);
    },
  );

  test(
    'structural getter null payloads are protocol errors after creation',
    () async {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (call) async {
            if (call.method == 'createPlayer') {
              return {
                'playerId': 7,
                'textureId': 7,
                'tracks': <Map<String, Object>>[
                  {
                    'fileId': 11,
                    'slot': 0,
                    'path': 'a.mp4',
                    'width': 1920,
                    'height': 1080,
                  },
                ],
              };
            }
            if (call.method == 'destroyPlayer') return null;
            return null;
          });
      final controller = NativePlayerController();

      await controller.createPlayer(const ['a.mp4']);
      addTearDown(controller.dispose);

      await expectLater(
        controller.getLayout(),
        throwsA(isA<NativeProtocolException>()),
      );
      await expectLater(
        controller.getTracks(),
        throwsA(isA<NativeProtocolException>()),
      );
      await expectLater(
        controller.getDiagnostics(),
        throwsA(isA<NativeProtocolException>()),
      );
    },
  );

  test('diagnostics rejects non-string keys with protocol error', () async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          if (call.method == 'createPlayer') {
            return {
              'playerId': 7,
              'textureId': 7,
              'tracks': <Map<String, Object>>[
                {
                  'fileId': 11,
                  'slot': 0,
                  'path': 'a.mp4',
                  'width': 1920,
                  'height': 1080,
                },
              ],
            };
          }
          if (call.method == 'getDiagnostics') {
            return {1: 'bad-key'};
          }
          if (call.method == 'destroyPlayer') return null;
          return null;
        });
    final controller = NativePlayerController();

    await controller.createPlayer(const ['a.mp4']);
    addTearDown(controller.dispose);

    await expectLater(
      controller.getDiagnostics(),
      throwsA(
        isA<NativeProtocolException>().having(
          (error) => error.reason,
          'reason',
          contains('diagnostics keys'),
        ),
      ),
    );
  });

  test(
    'disposed playback commands report diagnostics while staying tolerant',
    () async {
      final diagnostics = <String>[];
      final controller = NativePlayerController(
        onNoopCommand: (method, reason) => diagnostics.add('$method:$reason'),
      );

      await controller.dispose();
      await controller.play();
      await controller.seek(1000);
      await controller.resize(640, 360);
      await controller.setViewportBackgroundColor(0xFF000000);

      expect(diagnostics, [
        'play:controller disposed',
        'seek:controller disposed',
        'resize:controller disposed',
        'setViewportBackgroundColor:controller disposed',
      ]);
    },
  );
}
