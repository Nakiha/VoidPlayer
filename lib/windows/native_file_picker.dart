import 'package:flutter/services.dart';

class WindowsNativeFilePicker {
  WindowsNativeFilePicker._();

  static const _channel = MethodChannel('video_renderer');

  /// Open the Windows native file picker dialog (IFileDialog).
  /// Returns null if the user cancels.
  static Future<List<String>?> pickFiles({bool allowMultiple = true}) async {
    final result = await _channel.invokeMethod<List<dynamic>>('pickFiles', {
      'allowMultiple': allowMultiple,
    });
    if (result == null || result.isEmpty) return null;
    return result.cast<String>();
  }
}
