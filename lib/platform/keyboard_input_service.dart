import 'package:flutter/services.dart';

typedef KeyboardEventHandler = bool Function(KeyEvent event);

abstract class KeyboardInputService {
  bool get isControlPressed;
  void addHandler(KeyboardEventHandler handler);
  void removeHandler(KeyboardEventHandler handler);
}

class FlutterKeyboardInputService implements KeyboardInputService {
  const FlutterKeyboardInputService();

  @override
  bool get isControlPressed => HardwareKeyboard.instance.isControlPressed;

  @override
  void addHandler(KeyboardEventHandler handler) {
    HardwareKeyboard.instance.addHandler(handler);
  }

  @override
  void removeHandler(KeyboardEventHandler handler) {
    HardwareKeyboard.instance.removeHandler(handler);
  }
}
