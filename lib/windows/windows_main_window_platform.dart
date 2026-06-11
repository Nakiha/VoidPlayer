import 'dart:ui';

import 'package:window_manager/window_manager.dart';

import '../platform/main_window_platform.dart';
import 'win32ffi.dart';

class WindowsMainWindowPlatform implements MainWindowPlatform {
  const WindowsMainWindowPlatform();

  @override
  Future<void> setFullScreen(bool fullScreen) async {
    if (Win32FFI.setBorderlessFullScreen(fullScreen)) return;
    await windowManager.setFullScreen(fullScreen);
  }

  @override
  Future<Rect> getBounds() => windowManager.getBounds();
}
