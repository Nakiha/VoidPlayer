import 'package:flutter/services.dart';

abstract interface class NativeFilePicker {
  bool get isAvailable;

  Future<List<String>?> pickFiles({bool allowMultiple = true});
}

class MethodChannelNativeFilePicker implements NativeFilePicker {
  const MethodChannelNativeFilePicker([
    this._channel = const MethodChannel('video_renderer'),
  ]);

  final MethodChannel _channel;

  @override
  bool get isAvailable => true;

  @override
  Future<List<String>?> pickFiles({bool allowMultiple = true}) async {
    final result = await _channel.invokeMethod<List<dynamic>>('pickFiles', {
      'allowMultiple': allowMultiple,
    });
    if (result == null || result.isEmpty) return null;
    return result.cast<String>();
  }
}

class UnsupportedNativeFilePicker implements NativeFilePicker {
  const UnsupportedNativeFilePicker({
    this.message = 'Native file picking is not available on this platform yet.',
  });

  final String message;

  @override
  bool get isAvailable => false;

  @override
  Future<List<String>?> pickFiles({bool allowMultiple = true}) {
    throw UnsupportedError(message);
  }
}
