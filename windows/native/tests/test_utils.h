#pragma once
#include <windows.h>
#include <string>
#include <utility>
#include "video_renderer/d3d11/device.h"

namespace vr::test {

// Creates a hidden window for D3D11 testing. Each call creates a new window.
// Window class is registered once per process.
HWND create_hidden_window(int width = 800, int height = 600);

// Destroys a window created by create_hidden_window.
inline void destroy_window(HWND hwnd) {
    if (hwnd) DestroyWindow(hwnd);
}

// Creates a D3D11 device + hidden window pair for testing.
// Caller owns both pointers and must call cleanup_test_device() when done.
std::pair<vr::D3D11Device*, HWND> create_test_device(int width = 800, int height = 600);

// Cleans up a D3D11 device and window created by create_test_device().
void cleanup_test_device(vr::D3D11Device* dev, HWND hwnd);

// Injectable mock time source for deterministic clock tests.
struct MockTimeSource {
    int64_t t = 0;
    int64_t operator()() const { return t; }
};

// Returns the VIDEO_TEST_DIR path as a string.
inline std::string video_test_dir() {
    return std::string(VIDEO_TEST_DIR);
}

} // namespace vr::test
