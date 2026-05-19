import 'dart:ui';

import 'package:window_manager/window_manager.dart';

abstract class MainWindowPlatform {
  Future<void> setFullScreen(bool fullScreen);
  Future<Rect> getBounds();
}

class WindowManagerMainWindowPlatform implements MainWindowPlatform {
  const WindowManagerMainWindowPlatform();

  @override
  Future<void> setFullScreen(bool fullScreen) {
    return windowManager.setFullScreen(fullScreen);
  }

  @override
  Future<Rect> getBounds() => windowManager.getBounds();
}
