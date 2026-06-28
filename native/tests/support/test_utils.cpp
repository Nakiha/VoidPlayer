#include "test_utils.h"

namespace vr::test {

static bool g_window_class_registered = false;
static const wchar_t kWindowClassName[] = L"VRTestWnd";

HWND create_hidden_window(int width, int height) {
    if (!g_window_class_registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = kWindowClassName;
        RegisterClassExW(&wc);
        g_window_class_registered = true;
    }

    HWND hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        L"VR Test",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        width, height,
        nullptr, nullptr,
        GetModuleHandle(nullptr), nullptr
    );
    return hwnd;
}

} // namespace vr::test
