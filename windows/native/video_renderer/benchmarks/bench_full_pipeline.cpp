#include "bench_common.h"
#include <chrono>
#include <string>
#include <iostream>

#include <windows.h>
#include "video_renderer/renderer.h"

static LRESULT CALLBACK BenchWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BenchResult bench_full_pipeline(const std::string& path) {
    BenchResult result;
    result.name = "Stage 5: Full Pipeline (Renderer + Present)";

    // Create hidden Win32 window for D3D11 swap chain
    WNDCLASSW wc = {};
    wc.lpfnWndProc = BenchWndProc;
    wc.lpszClassName = L"VRBenchWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"VRBenchWnd", L"VR Bench",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        1920, 1080, nullptr, nullptr, nullptr, nullptr);
    if (!hwnd) {
        std::cerr << "Failed to create bench window\n";
        return result;
    }

    vr::Renderer renderer;
    vr::RendererConfig config;
    config.hwnd = hwnd;
    config.width = 1920;
    config.height = 1080;
    config.video_paths = { path };
    config.use_hardware_decode = true;

    if (!renderer.initialize(config)) {
        std::cerr << "Renderer::initialize failed\n";
        DestroyWindow(hwnd);
        return result;
    }

    int64_t duration_us = renderer.duration_us();
    size_t track_count = renderer.track_count();

    auto t0 = std::chrono::high_resolution_clock::now();
    renderer.play();

    // Wait for playback to reach end of video.
    // Renderer does not auto-stop; detect completion via PTS >= duration.
    int64_t timeout_ms = (duration_us / 1000) * 3 + 5000;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (renderer.is_playing()) {
        MSG msg;
        while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&msg);
        }

        if (duration_us > 0 && renderer.current_pts_us() >= duration_us) {
            break;
        }

        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "Stage 5: timeout after " << timeout_ms << " ms\n";
            break;
        }

        Sleep(1);
    }

    auto t1 = std::chrono::high_resolution_clock::now();

    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.bytes_processed = 0;

    if (duration_us > 0 && result.elapsed_ms > 0) {
        double video_duration_s = duration_us / 1'000'000.0;
        double elapsed_s = result.elapsed_ms / 1000.0;
        result.fps = video_duration_s / elapsed_s;
        result.total_frames = static_cast<int>(video_duration_s * 30);
    }

    renderer.shutdown();
    DestroyWindow(hwnd);

    std::cout << "  Duration:   " << (duration_us / 1'000'000.0) << " s\n";
    std::cout << "  Tracks:     " << track_count << "\n";

    return result;
}
