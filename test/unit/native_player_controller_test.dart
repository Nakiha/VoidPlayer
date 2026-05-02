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
    expect(await controller.duration(), 0);
    expect(await controller.isPlaying(), isFalse);
  });

  test('structural commands require a created player', () async {
    final controller = NativePlayerController();

    await expectLater(controller.getLayout(), throwsStateError);
    await expectLater(controller.addTrack('next.mp4'), throwsStateError);
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
}
