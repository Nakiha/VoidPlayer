import 'package:flutter/services.dart';

import '../platform/system_accent_watcher.dart';
import 'win32ffi.dart';

class WindowsSystemAccentWatcher implements SystemAccentWatcher {
  WindowsSystemAccentWatcher({required this.onChanged});

  static const MethodChannel _channel = MethodChannel(
    'void_player/window_bootstrap',
  );

  final ValueChanged<Color> onChanged;
  bool _disposed = false;

  @override
  void start() {
    _channel.setMethodCallHandler(_handleMethodCall);
  }

  @override
  void dispose() {
    _disposed = true;
    _channel.setMethodCallHandler(null);
  }

  Future<void> _handleMethodCall(MethodCall call) async {
    if (_disposed || call.method != 'systemAccentColorChanged') return;
    onChanged(Color(Win32FFI.getDwmAccentColorArgb()));
  }
}
