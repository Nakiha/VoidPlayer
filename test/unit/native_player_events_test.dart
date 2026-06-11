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
}
