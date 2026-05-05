import 'dart:ui';

import 'package:window_manager/window_manager.dart';

import '../win32ffi.dart';

abstract class MainWindowPlatform {
  Future<void> setFullScreen(bool fullScreen);
  Future<Rect> getBounds();
}

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
