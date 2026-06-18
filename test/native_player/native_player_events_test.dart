import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/video_renderer_controller.dart';

void main() {
  test('native event parser treats non-string event type as unknown', () {
    final event = NativePlayerEvent.fromMap(const {
      'schemaVersion': 1,
      'sequence': 7,
      'type': 123,
      'timestampUs': 456,
    });

    expect(event.schemaVersion, 1);
    expect(event.sequence, 7);
    expect(event.rawType, isEmpty);
    expect(event.type, NativePlayerEventType.unknown);
    expect(event.timestampUs, 456);
  });

  test('native event parser reads native compositor state', () {
    final event = NativePlayerEvent.fromMap(const {
      'schemaVersion': 1,
      'sequence': 8,
      'type': 'nativeCompositorState',
      'timestampUs': 456,
      'nativeCompositorActive': true,
      'nativeCompositorRequested': true,
      'nativeCompositorEDREnabled': true,
      'nativeCompositorMode': 'native-compositor-edr',
      'nativeCompositorReason': 'auto-hdr-track',
    });

    expect(event.type, NativePlayerEventType.nativeCompositorState);
    expect(event.nativeCompositorActive, isTrue);
    expect(event.nativeCompositorRequested, isTrue);
    expect(event.nativeCompositorEDREnabled, isTrue);
    expect(event.nativeCompositorMode, 'native-compositor-edr');
    expect(event.nativeCompositorReason, 'auto-hdr-track');
  });

  test('native event parser reads playback clock state', () {
    final event = NativePlayerEvent.fromMap(const {
      'schemaVersion': 1,
      'sequence': 9,
      'type': 'playbackClock',
      'timestampUs': 456,
      'ptsUs': 123000,
      'durationUs': 2000000,
      'isPlaying': true,
      'playbackSpeed': 1.5,
    });

    expect(event.type, NativePlayerEventType.playbackClock);
    expect(event.ptsUs, 123000);
    expect(event.durationUs, 2000000);
    expect(event.isPlaying, isTrue);
    expect(event.playbackSpeed, 1.5);
  });
}
