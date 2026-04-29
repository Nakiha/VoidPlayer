/// Minimal Win32 FFI bindings for window discovery, positioning, and input
/// state checks.
library;

import 'dart:ffi';
import 'dart:ui' show PlatformDispatcher, Rect;
import 'package:ffi/ffi.dart';

// ---------------------------------------------------------------------------
// Native function typedefs
// ---------------------------------------------------------------------------

typedef _GetForegroundWindowNative = IntPtr Function();
typedef _GetForegroundWindowDart = int Function();

typedef _FindWindowWNative =
    IntPtr Function(Pointer<Utf16> lpClassName, Pointer<Utf16> lpWindowName);
typedef _FindWindowWDart =
    int Function(Pointer<Utf16> lpClassName, Pointer<Utf16> lpWindowName);

typedef _GetWindowRectNative =
    Int32 Function(IntPtr hWnd, Pointer<RECT> lpRect);
typedef _GetWindowRectDart = int Function(int hWnd, Pointer<RECT> lpRect);

typedef _MoveWindowNative =
    Int32 Function(
      IntPtr hWnd,
      Int32 X,
      Int32 Y,
      Int32 nWidth,
      Int32 nHeight,
      Int32 bRepaint,
    );
typedef _MoveWindowDart =
    int Function(int hWnd, int X, int Y, int nWidth, int nHeight, int bRepaint);

typedef _SetWindowPosNative =
    Int32 Function(
      IntPtr hWnd,
      IntPtr hWndInsertAfter,
      Int32 X,
      Int32 Y,
      Int32 cx,
      Int32 cy,
      Uint32 uFlags,
    );
typedef _SetWindowPosDart =
    int Function(
      int hWnd,
      int hWndInsertAfter,
      int X,
      int Y,
      int cx,
      int cy,
      int uFlags,
    );

typedef _MonitorFromWindowNative = IntPtr Function(IntPtr hwnd, Uint32 dwFlags);
typedef _MonitorFromWindowDart = int Function(int hwnd, int dwFlags);

typedef _GetMonitorInfoWNative =
    Int32 Function(IntPtr hMonitor, Pointer<MONITORINFO> lpmi);
typedef _GetMonitorInfoWDart =
    int Function(int hMonitor, Pointer<MONITORINFO> lpmi);

typedef _IsWindowNative = Int32 Function(IntPtr hWnd);
typedef _IsWindowDart = int Function(int hWnd);

typedef _GetWindowTextWNative =
    Int32 Function(IntPtr hWnd, Pointer<Utf16> lpString, Int32 nMaxCount);
typedef _GetWindowTextWDart =
    int Function(int hWnd, Pointer<Utf16> lpString, int nMaxCount);

typedef _SetWindowTextWNative =
    Int32 Function(IntPtr hWnd, Pointer<Utf16> lpString);
typedef _SetWindowTextWDart = int Function(int hWnd, Pointer<Utf16> lpString);

typedef _GetCurrentProcessIdNative = Uint32 Function();
typedef _GetCurrentProcessIdDart = int Function();

typedef _GetWindowThreadProcessIdNative =
    Uint32 Function(IntPtr hWnd, Pointer<Uint32> lpdwProcessId);
typedef _GetWindowThreadProcessIdDart =
    int Function(int hWnd, Pointer<Uint32> lpdwProcessId);

typedef _GetClassNameWNative =
    Int32 Function(IntPtr hWnd, Pointer<Utf16> lpClassName, Int32 nMaxCount);
typedef _GetClassNameWDart =
    int Function(int hWnd, Pointer<Utf16> lpClassName, int nMaxCount);

typedef _EnumWindowsNative =
    Int32 Function(
      Pointer<NativeFunction<_EnumWindowsCallbackNative>>,
      IntPtr lParam,
    );
typedef _EnumWindowsDart =
    int Function(
      Pointer<NativeFunction<_EnumWindowsCallbackNative>>,
      int lParam,
    );
typedef _EnumWindowsCallbackNative = Int32 Function(IntPtr hWnd, IntPtr lParam);

typedef _GetAsyncKeyStateNative = Int16 Function(Int32 vKey);
typedef _GetAsyncKeyStateDart = int Function(int vKey);

typedef _GetWindowLongPtrWNative = IntPtr Function(IntPtr hWnd, Int32 nIndex);
typedef _GetWindowLongPtrWDart = int Function(int hWnd, int nIndex);

typedef _SetWindowLongPtrWNative =
    IntPtr Function(IntPtr hWnd, Int32 nIndex, IntPtr dwNewLong);
typedef _SetWindowLongPtrWDart =
    int Function(int hWnd, int nIndex, int dwNewLong);

typedef _GetSystemMetricsNative = Int32 Function(Int32 nIndex);
typedef _GetSystemMetricsDart = int Function(int nIndex);

// ---------------------------------------------------------------------------
// Structs
// ---------------------------------------------------------------------------

final class RECT extends Struct {
  @Int32()
  external int left;
  @Int32()
  external int top;
  @Int32()
  external int right;
  @Int32()
  external int bottom;

  Rect toDartRect() => Rect.fromLTWH(
    left.toDouble(),
    top.toDouble(),
    (right - left).toDouble(),
    (bottom - top).toDouble(),
  );
}

/// sizeof(MONITORINFO) = 4 + 16 + 16 + 4 = 40 bytes.
final class MONITORINFO extends Struct {
  @Uint32()
  external int cbSize;
  external RECT rcMonitor;
  external RECT rcWork;
  @Uint32()
  external int dwFlags;
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const int _swpNoZOrder = 0x0004;
const int _swpNoMove = 0x0002;
const int _swpNoSize = 0x0001;
const int _swpNoActivate = 0x0010;
const int _swpShowWindow = 0x0040;
const int _swpFrameChanged = 0x0020;
const int _monitorDefaultToNearest = 0x00000002;
const int _vkLButton = 0x01;
const int _vkRButton = 0x02;
const int _smCyCaption = 4;
const int _smCyFrame = 33;
const int _smCyPaddedBorder = 92;
const int _gwlExStyle = -20;
const int _wsExToolWindow = 0x00000080;
const int _wsExAppWindow = 0x00040000;

/// Window class name used by the Flutter runner for the main window.
const String kMainWindowClass = 'FLUTTER_RUNNER_WIN32_WINDOW';

// ---------------------------------------------------------------------------
// DLL & bindings
// ---------------------------------------------------------------------------

final _user32 = DynamicLibrary.open('user32.dll');
final _kernel32 = DynamicLibrary.open('kernel32.dll');

final _getForegroundWindow = _user32
    .lookupFunction<_GetForegroundWindowNative, _GetForegroundWindowDart>(
      'GetForegroundWindow',
    );

final _findWindowW = _user32
    .lookupFunction<_FindWindowWNative, _FindWindowWDart>('FindWindowW');

final _getWindowRect = _user32
    .lookupFunction<_GetWindowRectNative, _GetWindowRectDart>('GetWindowRect');

final _moveWindow = _user32.lookupFunction<_MoveWindowNative, _MoveWindowDart>(
  'MoveWindow',
);

final _setWindowPos = _user32
    .lookupFunction<_SetWindowPosNative, _SetWindowPosDart>('SetWindowPos');

final _monitorFromWindow = _user32
    .lookupFunction<_MonitorFromWindowNative, _MonitorFromWindowDart>(
      'MonitorFromWindow',
    );

final _getMonitorInfoW = _user32
    .lookupFunction<_GetMonitorInfoWNative, _GetMonitorInfoWDart>(
      'GetMonitorInfoW',
    );

final _isWindow = _user32.lookupFunction<_IsWindowNative, _IsWindowDart>(
  'IsWindow',
);

final _getWindowTextW = _user32
    .lookupFunction<_GetWindowTextWNative, _GetWindowTextWDart>(
      'GetWindowTextW',
    );

final _setWindowTextW = _user32
    .lookupFunction<_SetWindowTextWNative, _SetWindowTextWDart>(
      'SetWindowTextW',
    );

final _getCurrentProcessId = _kernel32
    .lookupFunction<_GetCurrentProcessIdNative, _GetCurrentProcessIdDart>(
      'GetCurrentProcessId',
    );

final _getWindowThreadProcessId = _user32
    .lookupFunction<
      _GetWindowThreadProcessIdNative,
      _GetWindowThreadProcessIdDart
    >('GetWindowThreadProcessId');

final _getClassNameW = _user32
    .lookupFunction<_GetClassNameWNative, _GetClassNameWDart>('GetClassNameW');

final _enumWindows = _user32
    .lookupFunction<_EnumWindowsNative, _EnumWindowsDart>('EnumWindows');

final _getAsyncKeyState = _user32
    .lookupFunction<_GetAsyncKeyStateNative, _GetAsyncKeyStateDart>(
      'GetAsyncKeyState',
    );

final _getWindowLongPtrW = _user32
    .lookupFunction<_GetWindowLongPtrWNative, _GetWindowLongPtrWDart>(
      'GetWindowLongPtrW',
    );

final _setWindowLongPtrW = _user32
    .lookupFunction<_SetWindowLongPtrWNative, _SetWindowLongPtrWDart>(
      'SetWindowLongPtrW',
    );

final _getSystemMetrics = _user32
    .lookupFunction<_GetSystemMetricsNative, _GetSystemMetricsDart>(
      'GetSystemMetrics',
    );

// ---------------------------------------------------------------------------
// Global state for EnumWindows callback (top-level for Pointer.fromFunction)
// ---------------------------------------------------------------------------

final List<int> _enumResult = [];
int _enumTargetPid = 0;
String? _enumClassFilter;

int _enumCurrentProcessWindowsCallback(int hwnd, int lParam) {
  final clsBuf = calloc.allocate<Utf16>(512);
  try {
    final len = _getClassNameW(hwnd, clsBuf.cast<Utf16>(), 256);
    final className = len > 0
        ? clsBuf.cast<Utf16>().toDartString(length: len)
        : '';
    final matchesClass = _enumClassFilter != null
        ? className == _enumClassFilter
        : true;
    if (matchesClass) {
      final pidBuf = calloc.allocate<Uint32>(4);
      try {
        _getWindowThreadProcessId(hwnd, pidBuf.cast<Uint32>());
        if (pidBuf.cast<Uint32>().value == _enumTargetPid) {
          _enumResult.add(hwnd);
        }
      } finally {
        calloc.free(pidBuf);
      }
    }
  } finally {
    calloc.free(clsBuf);
  }
  return 1; // continue
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

class Win32FFI {
  Win32FFI._();

  /// Returns the HWND of the current foreground window.
  static int getForegroundWindow() => _getForegroundWindow();

  /// Returns whether [hwnd] refers to a valid window.
  static bool isWindow(int hwnd) => hwnd != 0 && _isWindow(hwnd) != 0;

  /// Returns the window's outer rect (including title bar / borders).
  static Rect getWindowRect(int hwnd) {
    final buf = calloc.allocate<RECT>(40); // sizeof(RECT) = 16
    try {
      _getWindowRect(hwnd, buf.cast<RECT>());
      return buf.cast<RECT>().ref.toDartRect();
    } finally {
      calloc.free(buf);
    }
  }

  /// Returns the window title as a Dart string.
  static String getWindowText(int hwnd) {
    final buf = calloc.allocate<Utf16>(1024); // 512 wchar16s
    try {
      final len = _getWindowTextW(hwnd, buf.cast<Utf16>(), 512);
      return len > 0 ? buf.cast<Utf16>().toDartString(length: len) : '';
    } finally {
      calloc.free(buf);
    }
  }

  /// Returns the window class name as a Dart string.
  static String getWindowClassName(int hwnd) {
    final buf = calloc.allocate<Utf16>(1024);
    try {
      final len = _getClassNameW(hwnd, buf.cast<Utf16>(), 512);
      return len > 0 ? buf.cast<Utf16>().toDartString(length: len) : '';
    } finally {
      calloc.free(buf);
    }
  }

  /// Returns the owning process id for [hwnd], or 0 if it cannot be read.
  static int getWindowProcessId(int hwnd) {
    final pidBuf = calloc.allocate<Uint32>(4);
    try {
      _getWindowThreadProcessId(hwnd, pidBuf.cast<Uint32>());
      return pidBuf.cast<Uint32>().value;
    } finally {
      calloc.free(pidBuf);
    }
  }

  /// Returns whether [hwnd] is a top-level window of this process with [className].
  static bool isCurrentProcessWindowOfClass(int hwnd, String className) {
    if (!isWindow(hwnd)) return false;
    return getWindowProcessId(hwnd) == getCurrentProcessId() &&
        getWindowClassName(hwnd) == className;
  }

  /// Sets the window title.
  static void setWindowText(int hwnd, String text) {
    final ptr = text.toNativeUtf16();
    try {
      _setWindowTextW(hwnd, ptr);
    } finally {
      calloc.free(ptr);
    }
  }

  /// Moves and resizes the window in one call.
  static void moveWindow(int hwnd, int x, int y, int w, int h) {
    _moveWindow(hwnd, x, y, w, h, 1);
  }

  /// Positions and optionally resizes the window without changing Z-order.
  static void setWindowPos(
    int hwnd,
    int x,
    int y,
    int w,
    int h, {
    bool show = false,
  }) {
    _setWindowPos(
      hwnd,
      0, // HWND_TOP
      x,
      y,
      w,
      h,
      _swpNoZOrder | _swpNoActivate | (show ? _swpShowWindow : 0),
    );
  }

  /// Removes the window from the taskbar without hiding its rendering surface.
  static void hideFromTaskbar(int hwnd) {
    if (!isWindow(hwnd)) return;
    final style = _getWindowLongPtrW(hwnd, _gwlExStyle);
    final nextStyle = (style & ~_wsExAppWindow) | _wsExToolWindow;
    _setWindowLongPtrW(hwnd, _gwlExStyle, nextStyle);
    _setWindowPos(
      hwnd,
      0,
      0,
      0,
      0,
      0,
      _swpNoMove |
          _swpNoSize |
          _swpNoZOrder |
          _swpNoActivate |
          _swpFrameChanged,
    );
  }

  /// Returns the usable work-area rect of the monitor that contains [hwnd].
  static Rect getMonitorWorkArea(int hwnd) {
    final monitor = _monitorFromWindow(hwnd, _monitorDefaultToNearest);
    final info = calloc.allocate<MONITORINFO>(40); // sizeof(MONITORINFO) = 40
    try {
      final mi = info.cast<MONITORINFO>();
      mi.ref.cbSize = 40; // sizeof(MONITORINFO)
      _getMonitorInfoW(monitor, mi);
      return mi.ref.rcWork.toDartRect();
    } finally {
      calloc.free(info);
    }
  }

  /// Returns the current process ID.
  static int getCurrentProcessId() => _getCurrentProcessId();

  /// Returns whether the left mouse button is currently physically down.
  static bool isLeftMouseButtonDown() =>
      (_getAsyncKeyState(_vkLButton) & 0x8000) != 0;

  /// Returns whether the right mouse button is currently physically down.
  static bool isRightMouseButtonDown() =>
      (_getAsyncKeyState(_vkRButton) & 0x8000) != 0;

  /// Approximate top non-client height for an overlapped window.
  static int titleBarOffset() {
    final raw =
        _getSystemMetrics(_smCyCaption) +
        _getSystemMetrics(_smCyFrame) +
        _getSystemMetrics(_smCyPaddedBorder);
    if (raw <= 0) return 32;
    return raw.clamp(24, 64).toInt();
  }

  /// Finds a window by class name and/or title.
  /// Pass `null` for either parameter to act as a wildcard.
  static int findWindow({String? className, String? title}) {
    final clsPtr = className != null ? className.toNativeUtf16() : nullptr;
    final titlePtr = title != null ? title.toNativeUtf16() : nullptr;
    try {
      return _findWindowW(clsPtr, titlePtr);
    } finally {
      if (className != null) calloc.free(clsPtr);
      if (title != null) calloc.free(titlePtr);
    }
  }

  /// Finds top-level windows with [className] belonging to this process.
  static List<int> findCurrentProcessWindowsByClass(String className) {
    _enumResult.clear();
    _enumTargetPid = getCurrentProcessId();
    _enumClassFilter = className;
    final callback = Pointer.fromFunction<_EnumWindowsCallbackNative>(
      _enumCurrentProcessWindowsCallback,
      0,
    );
    _enumWindows(callback, 0);
    _enumClassFilter = null;
    return List.unmodifiable(_enumResult);
  }

  // -----------------------------------------------------------------------
  // Position helpers
  // -----------------------------------------------------------------------

  /// Computes a cascaded position for a related window relative to [parentRect],
  /// clamped within [monitorWorkArea].
  static Rect cascadePosition(
    Rect parentRect,
    Rect monitorWorkArea, {
    int offset = 32,
    int defaultWidth = 800,
    int defaultHeight = 600,
  }) {
    int x = parentRect.left.toInt() + offset;
    int y = parentRect.top.toInt() + offset;
    final int w = defaultWidth;
    final int h = defaultHeight;

    final int screenRight = monitorWorkArea.right.toInt();
    final int screenBottom = monitorWorkArea.bottom.toInt();
    final int screenLeft = monitorWorkArea.left.toInt();
    final int screenTop = monitorWorkArea.top.toInt();

    // If cascaded position would overflow, align to screen edge.
    if (x + w > screenRight) x = screenRight - w;
    if (y + h > screenBottom) y = screenBottom - h;
    if (x < screenLeft) x = screenLeft;
    if (y < screenTop) y = screenTop;

    return Rect.fromLTWH(
      x.toDouble(),
      y.toDouble(),
      w.toDouble(),
      h.toDouble(),
    );
  }

  /// Clamps [rect] so it stays fully inside [monitorWorkArea].
  static Rect ensureOnScreen(Rect rect, Rect monitorWorkArea) {
    double x = rect.left;
    double y = rect.top;
    final double w = rect.width;
    final double h = rect.height;

    if (x + w > monitorWorkArea.right) {
      x = monitorWorkArea.right - w;
    }
    if (y + h > monitorWorkArea.bottom) {
      y = monitorWorkArea.bottom - h;
    }
    if (x < monitorWorkArea.left) x = monitorWorkArea.left;
    if (y < monitorWorkArea.top) y = monitorWorkArea.top;

    return Rect.fromLTWH(x, y, w, h);
  }

  /// Checks whether [rect] overlaps with any connected display.
  static bool isRectOnScreen(Rect rect) {
    for (final display in PlatformDispatcher.instance.displays) {
      final w = display.size.width / display.devicePixelRatio;
      final h = display.size.height / display.devicePixelRatio;
      if (rect.overlaps(Rect.fromLTWH(0, 0, w, h))) return true;
    }
    return false;
  }
}
