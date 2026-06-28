#pragma once
#include <windows.h>
#include <string>

namespace vr::test {

// Creates a hidden native window for platform tests. Each call creates a new window.
// Window class is registered once per process.
HWND create_hidden_window(int width = 800, int height = 600);

// Destroys a window created by create_hidden_window.
inline void destroy_window(HWND hwnd) {
    if (hwnd) DestroyWindow(hwnd);
}

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
