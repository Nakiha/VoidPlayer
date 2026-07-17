import 'dart:ffi';

/// Win32 cursor and mouse-button injection for native input automation.
///
/// `user32.dll` is resolved lazily on first use, so importing this file on
/// non-Windows platforms is safe as long as these functions are never called.
final _user32 = DynamicLibrary.open('user32.dll');
const _mouseEventLeftDown = 0x0002;
const _mouseEventLeftUp = 0x0004;
const _mouseEventRightDown = 0x0008;
const _mouseEventRightUp = 0x0010;
const _mouseEventWheel = 0x0800;
const _keyEventKeyUp = 0x0002;
const _mapVkToVsc = 0;
const _gwChild = 5;
const _wmMouseMove = 0x0200;
const _wmRightButtonDown = 0x0204;
const _wmRightButtonUp = 0x0205;
const _wmKeyDown = 0x0100;
const _wmKeyUp = 0x0101;
const _mkRightButton = 0x0002;
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
final _keybdEvent = _user32
    .lookupFunction<
      Void Function(
        Uint8 virtualKey,
        Uint8 scanCode,
        Uint32 flags,
        IntPtr extraInfo,
      ),
      void Function(int virtualKey, int scanCode, int flags, int extraInfo)
    >('keybd_event');
final _mapVirtualKey = _user32
    .lookupFunction<
      Uint32 Function(Uint32 code, Uint32 mapType),
      int Function(int code, int mapType)
    >('MapVirtualKeyW');
final _getWindow = _user32
    .lookupFunction<
      IntPtr Function(IntPtr hwnd, Uint32 command),
      int Function(int hwnd, int command)
    >('GetWindow');
final _postMessage = _user32
    .lookupFunction<
      Int32 Function(IntPtr hwnd, Uint32 message, IntPtr wParam, IntPtr lParam),
      int Function(int hwnd, int message, int wParam, int lParam)
    >('PostMessageW');

void nativeSetCursorPos(int x, int y) => _setCursorPos(x, y);

void nativeMouseLeftDown() => _mouseEvent(_mouseEventLeftDown, 0, 0, 0, 0);

void nativeMouseLeftUp() => _mouseEvent(_mouseEventLeftUp, 0, 0, 0, 0);

void nativeMouseRightDown() => _mouseEvent(_mouseEventRightDown, 0, 0, 0, 0);

void nativeMouseRightUp() => _mouseEvent(_mouseEventRightUp, 0, 0, 0, 0);

void nativeMouseWheel(int delta) =>
    _mouseEvent(_mouseEventWheel, 0, 0, delta & 0xffffffff, 0);

void nativeKeyDown(int virtualKey) =>
    _keybdEvent(virtualKey, _mapVirtualKey(virtualKey, _mapVkToVsc), 0, 0);

void nativeKeyUp(int virtualKey) => _keybdEvent(
  virtualKey,
  _mapVirtualKey(virtualKey, _mapVkToVsc),
  _keyEventKeyUp,
  0,
);

int nativeFlutterChildWindow(int root) {
  return root == 0 ? 0 : _getWindow(root, _gwChild);
}

int _mouseLParam(int x, int y) => (x & 0xffff) | ((y & 0xffff) << 16);

bool nativePostRightButtonDown(int hwnd, int x, int y) =>
    _postMessage(
      hwnd,
      _wmRightButtonDown,
      _mkRightButton,
      _mouseLParam(x, y),
    ) !=
    0;

bool nativePostRightButtonMove(int hwnd, int x, int y) =>
    _postMessage(hwnd, _wmMouseMove, _mkRightButton, _mouseLParam(x, y)) != 0;

bool nativePostRightButtonUp(int hwnd, int x, int y) =>
    _postMessage(hwnd, _wmRightButtonUp, 0, _mouseLParam(x, y)) != 0;

bool nativePostKeyDown(int hwnd, int virtualKey) {
  final scanCode = _mapVirtualKey(virtualKey, _mapVkToVsc);
  return _postMessage(hwnd, _wmKeyDown, virtualKey, 1 | (scanCode << 16)) != 0;
}

bool nativePostKeyUp(int hwnd, int virtualKey) {
  final scanCode = _mapVirtualKey(virtualKey, _mapVkToVsc);
  return _postMessage(
        hwnd,
        _wmKeyUp,
        virtualKey,
        1 | (scanCode << 16) | (1 << 30) | (1 << 31),
      ) !=
      0;
}
