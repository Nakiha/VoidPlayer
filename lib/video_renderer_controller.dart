import 'package:flutter/services.dart';

class VideoRendererController {
  static const MethodChannel _channel = MethodChannel('video_renderer');

  int? _textureId;
  int? get textureId => _textureId;

  Future<int> createRenderer(
    List<String> videoPaths, {
    int width = 1920,
    int height = 1080,
  }) async {
    _textureId = await _channel.invokeMethod<int>('createRenderer', {
      'videoPaths': videoPaths,
      'width': width,
      'height': height,
    });
    return _textureId!;
  }

  Future<void> play() => _channel.invokeMethod<void>('play');

  Future<void> pause() => _channel.invokeMethod<void>('pause');

  Future<void> resume() => _channel.invokeMethod<void>('resume');

  Future<void> seek(int ptsUs) => _channel.invokeMethod<void>('seek', {
        'ptsUs': ptsUs,
      });

  Future<void> setSpeed(double speed) =>
      _channel.invokeMethod<void>('setSpeed', {
        'speed': speed,
      });

  Future<void> stepForward() => _channel.invokeMethod<void>('stepForward');

  Future<void> stepBackward() => _channel.invokeMethod<void>('stepBackward');

  Future<int> currentPts() async {
    return await _channel.invokeMethod<int>('currentPts') ?? 0;
  }

  Future<int> duration() async {
    return await _channel.invokeMethod<int>('duration') ?? 0;
  }

  Future<bool> isPlaying() async {
    return await _channel.invokeMethod<bool>('isPlaying') ?? false;
  }

  Future<void> dispose() async {
    if (_textureId != null) {
      await _channel.invokeMethod<void>('destroyRenderer');
      _textureId = null;
    }
  }
}
