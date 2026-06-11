import 'dart:ffi';

/// Win32 cursor and mouse-button injection for native input automation.
///
/// `user32.dll` is resolved lazily on first use, so importing this file on
/// non-Windows platforms is safe as long as these functions are never called.
final _user32 = DynamicLibrary.open('user32.dll');
const _mouseEventLeftDown = 0x0002;
const _mouseEventLeftUp = 0x0004;
final _setCursorPos = _user32
    .lookupFunction<
      Int32 Function(Int32 x, Int32 y),
      int Function(int x, int y)
    >('SetCursorPos');
final _mouseEvent = _user32
    .lookupFunction<
      Void Function(
        Uint32 dwFlags,
        Uint32 dx,
        Uint32 dy,
        Uint32 dwData,
        IntPtr dwExtraInfo,
      ),
      void Function(int dwFlags, int dx, int dy, int dwData, int dwExtraInfo)
    >('mouse_event');

void nativeSetCursorPos(int x, int y) => _setCursorPos(x, y);

void nativeMouseLeftDown() => _mouseEvent(_mouseEventLeftDown, 0, 0, 0, 0);

void nativeMouseLeftUp() => _mouseEvent(_mouseEventLeftUp, 0, 0, 0, 0);
