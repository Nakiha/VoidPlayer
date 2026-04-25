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

std::pair<vr::D3D11Device*, HWND> create_test_device(int width, int height) {
    vr::D3D11Device* dev = new vr::D3D11Device();
    HWND hwnd = create_hidden_window(width, height);
    dev->initialize(hwnd, width, height);
    return {dev, hwnd};
}

void cleanup_test_device(vr::D3D11Device* dev, HWND hwnd) {
    if (dev) {
        dev->shutdown();
        delete dev;
    }
    destroy_window(hwnd);
}

} // namespace vr::test
