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

  test('native event parser reads renderer-owned presentation state', () {
    final event = NativePlayerEvent.fromMap(const {
      'schemaVersion': 1,
      'sequence': 8,
      'type': 'rendererOwnedPresentationState',
      'timestampUs': 456,
      'rendererOwnedPresentationActive': true,
      'rendererOwnedRendererActive': true,
      'rendererOwnedPresentationRequested': true,
      'rendererOwnedEDROutputEnabled': true,
      'rendererOwnedPresentationMode': 'renderer-owned-wgpu-edr',
      'rendererOwnedPresentationReason': 'auto-hdr-track',
    });

    expect(event.type, NativePlayerEventType.rendererOwnedPresentationState);
    expect(event.rendererOwnedPresentationActive, isTrue);
    expect(event.rendererOwnedRunnerLayerActive, isFalse);
    expect(event.rendererOwnedRendererActive, isTrue);
    expect(event.rendererOwnedPresentationRequested, isTrue);
    expect(event.rendererOwnedEDROutputEnabled, isTrue);
    expect(event.rendererOwnedPresentationMode, 'renderer-owned-wgpu-edr');
    expect(event.rendererOwnedPresentationReason, 'auto-hdr-track');
    expect(event.nativeCompositorActive, isTrue);
    expect(event.nativeCompositorRunnerLayerActive, isFalse);
    expect(event.nativeCompositorRendererOwnedActive, isTrue);
    expect(event.nativeCompositorRequested, isTrue);
    expect(event.nativeCompositorEDREnabled, isTrue);
    expect(event.nativeCompositorMode, 'renderer-owned-wgpu-edr');
    expect(event.nativeCompositorReason, 'auto-hdr-track');
  });

  test('native event parser still accepts native compositor state', () {
    final event = NativePlayerEvent.fromMap(const {
      'schemaVersion': 1,
      'sequence': 8,
      'type': 'nativeCompositorState',
      'timestampUs': 456,
      'nativeCompositorActive': true,
      'nativeCompositorRunnerLayerActive': true,
      'nativeCompositorRendererOwnedActive': false,
      'nativeCompositorRequested': true,
      'nativeCompositorMode': 'dcomp-native-sdr',
    });

    expect(event.type, NativePlayerEventType.nativeCompositorState);
    expect(event.nativeCompositorActive, isTrue);
    expect(event.nativeCompositorRunnerLayerActive, isTrue);
    expect(event.rendererOwnedPresentationActive, isTrue);
    expect(event.rendererOwnedRunnerLayerActive, isTrue);
    expect(event.rendererOwnedPresentationMode, 'dcomp-native-sdr');
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
