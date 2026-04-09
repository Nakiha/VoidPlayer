#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/renderer.h"
#include <thread>
#include <chrono>
#include <dxgi.h>

using namespace vr;
using namespace vr::test;

// Helper: get the default DXGI adapter for headless mode.
// Returns nullptr if no adapter is available.
static IDXGIAdapter* get_default_adapter() {
    static Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (adapter) return adapter.Get();

    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), &factory);
    if (FAILED(hr)) return nullptr;

    hr = factory->EnumAdapters(0, &adapter);
    if (FAILED(hr)) return nullptr;

    return adapter.Get();
}

// =============================================================================
// Windowed-mode tests (software decode)
// =============================================================================

TEST_CASE("Renderer: initialize with single H264 file", "[renderer]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    REQUIRE(renderer.initialize(config));
    REQUIRE(renderer.is_initialized());
    REQUIRE(renderer.track_count() == 1);
    REQUIRE(renderer.duration_us() > 0);

    renderer.shutdown();
    REQUIRE_FALSE(renderer.is_initialized());
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("Renderer: play and check PTS advances", "[renderer]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    renderer.initialize(config);
    renderer.play();

    // Wait for playback to start
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int64_t pts = renderer.current_pts_us();
    REQUIRE(pts > 0);

    renderer.shutdown();
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("Renderer: pause and play", "[renderer]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    renderer.initialize(config);
    renderer.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    renderer.pause();
    int64_t pts_at_pause = renderer.current_pts_us();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int64_t pts_after_wait = renderer.current_pts_us();

    // PTS should not advance while paused
    REQUIRE(std::abs(pts_after_wait - pts_at_pause) < 10000);

    renderer.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int64_t pts_after_play = renderer.current_pts_us();
    REQUIRE(pts_after_play > pts_at_pause);

    renderer.shutdown();
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("Renderer: multi-track initialization", "[renderer]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = {
        video_test_dir() + "/h264_9s_1920x1080.mp4",
        video_test_dir() + "/h265_10s_1920x1080.mp4"
    };
    config.hwnd = create_hidden_window(1280, 480);
    config.width = 1280;
    config.height = 480;
    config.use_hardware_decode = false;

    REQUIRE(renderer.initialize(config));
    REQUIRE(renderer.track_count() == 2);

    renderer.shutdown();
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("Renderer: shutdown without play is safe", "[renderer]") {
    Renderer renderer;
    renderer.shutdown();
    REQUIRE_FALSE(renderer.is_initialized());
}

// =============================================================================
// Headless-mode tests (hardware decode — mirrors Flutter plugin setup)
// =============================================================================

TEST_CASE("Renderer: headless hw decode initialize", "[renderer][hw]") {
    auto* adapter = get_default_adapter();
    REQUIRE(adapter != nullptr);

    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.headless = true;
    config.dxgi_adapter = adapter;
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = true;

    REQUIRE(renderer.initialize(config));
    REQUIRE(renderer.is_initialized());
    REQUIRE(renderer.track_count() == 1);
    REQUIRE(renderer.duration_us() > 0);

    // Verify shared texture is available for Flutter consumption
    REQUIRE(renderer.shared_texture() != nullptr);
    REQUIRE(renderer.shared_texture_handle() != nullptr);

    renderer.shutdown();
    REQUIRE_FALSE(renderer.is_initialized());
}

TEST_CASE("Renderer: headless hw decode play", "[renderer][hw]") {
    auto* adapter = get_default_adapter();
    REQUIRE(adapter != nullptr);

    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.headless = true;
    config.dxgi_adapter = adapter;
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = true;

    REQUIRE(renderer.initialize(config));
    renderer.play();

    // Wait for playback to start and a few frames to be decoded/rendered
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    int64_t pts = renderer.current_pts_us();
    REQUIRE(pts > 0);

    // Verify shared texture remains valid during playback
    REQUIRE(renderer.shared_texture() != nullptr);
    REQUIRE(renderer.shared_texture_handle() != nullptr);

    renderer.shutdown();
}

TEST_CASE("Renderer: headless hw decode multi-track", "[renderer][hw]") {
    auto* adapter = get_default_adapter();
    REQUIRE(adapter != nullptr);

    Renderer renderer;

    RendererConfig config;
    config.video_paths = {
        video_test_dir() + "/h264_9s_1920x1080.mp4",
        video_test_dir() + "/h265_10s_1920x1080.mp4"
    };
    config.headless = true;
    config.dxgi_adapter = adapter;
    config.width = 1280;
    config.height = 480;
    config.use_hardware_decode = true;

    REQUIRE(renderer.initialize(config));
    REQUIRE(renderer.track_count() == 2);

    renderer.shutdown();
}
