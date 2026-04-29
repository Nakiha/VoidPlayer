#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/renderer.h"
#include <thread>
#include <chrono>
#include <cstdint>
#include <filesystem>
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

struct CaptureStats {
    uint64_t hash = 1469598103934665603ull;
    double avg_luma = 0.0;
    double non_black_ratio = 0.0;
    int width = 0;
    int height = 0;
};

static CaptureStats analyze_bgra(const std::vector<uint8_t>& bgra, int width, int height) {
    CaptureStats stats;
    stats.width = width;
    stats.height = height;
    if (bgra.empty() || width <= 0 || height <= 0) return stats;

    uint64_t luma_sum = 0;
    size_t non_black = 0;
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < pixels; ++i) {
        const size_t off = i * 4;
        const uint8_t b = bgra[off + 0];
        const uint8_t g = bgra[off + 1];
        const uint8_t r = bgra[off + 2];
        stats.hash ^= b;
        stats.hash *= 1099511628211ull;
        stats.hash ^= g;
        stats.hash *= 1099511628211ull;
        stats.hash ^= r;
        stats.hash *= 1099511628211ull;
        const int luma = (77 * r + 150 * g + 29 * b) >> 8;
        luma_sum += static_cast<uint64_t>(luma);
        if (r > 8 || g > 8 || b > 8) {
            ++non_black;
        }
    }
    stats.avg_luma = static_cast<double>(luma_sum) / static_cast<double>(pixels);
    stats.non_black_ratio = static_cast<double>(non_black) / static_cast<double>(pixels);
    return stats;
}

static CaptureStats wait_for_non_black_capture(Renderer& renderer,
                                               std::chrono::milliseconds timeout =
                                                   std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    CaptureStats latest;
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<uint8_t> bgra;
        int width = 0;
        int height = 0;
        if (renderer.capture_front_buffer(bgra, width, height)) {
            latest = analyze_bgra(bgra, width, height);
            if (latest.non_black_ratio >= 0.01 && latest.avg_luma >= 4.0) {
                return latest;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return latest;
}

static CaptureStats wait_for_changed_non_black_capture(Renderer& renderer,
                                                       uint64_t previous_hash,
                                                       std::chrono::milliseconds timeout =
                                                           std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    CaptureStats latest;
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<uint8_t> bgra;
        int width = 0;
        int height = 0;
        if (renderer.capture_front_buffer(bgra, width, height)) {
            latest = analyze_bgra(bgra, width, height);
            if (latest.hash != previous_hash &&
                latest.non_black_ratio >= 0.01 &&
                latest.avg_luma >= 4.0) {
                return latest;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return latest;
}

static void require_visual_frame(const CaptureStats& stats) {
    INFO("capture " << stats.width << "x" << stats.height
                    << " avg_luma=" << stats.avg_luma
                    << " non_black=" << stats.non_black_ratio
                    << " hash=" << stats.hash);
    REQUIRE(stats.width > 0);
    REQUIRE(stats.height > 0);
    REQUIRE(stats.non_black_ratio >= 0.01);
    REQUIRE(stats.avg_luma >= 4.0);
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

TEST_CASE("Renderer: duplicate initialize is rejected without tearing down the running renderer", "[renderer]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    REQUIRE(renderer.initialize(config));
    REQUIRE_FALSE(renderer.initialize(config));
    REQUIRE(renderer.is_initialized());
    REQUIRE(renderer.track_count() == 1);

    renderer.shutdown();
    REQUIRE_FALSE(renderer.is_initialized());
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("Renderer: failed initialize rolls back resources and allows retry", "[renderer]") {
    Renderer renderer;

    RendererConfig bad_config;
    bad_config.video_paths = { video_test_dir() + "/missing-video.mp4" };
    bad_config.hwnd = create_hidden_window(640, 480);
    bad_config.width = 640;
    bad_config.height = 480;
    bad_config.use_hardware_decode = false;

    REQUIRE_FALSE(renderer.initialize(bad_config));
    REQUIRE_FALSE(renderer.is_initialized());
    REQUIRE(renderer.track_count() == 0);

    RendererConfig good_config = bad_config;
    good_config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };

    REQUIRE(renderer.initialize(good_config));
    REQUIRE(renderer.is_initialized());
    REQUIRE(renderer.track_count() == 1);

    renderer.shutdown();
    REQUIRE_FALSE(renderer.is_initialized());
    destroy_window(static_cast<HWND>(bad_config.hwnd));
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

TEST_CASE("Renderer: headless HEVC paused exact seek updates captured frame", "[renderer][hw][seek][visual]") {
    auto* adapter = get_default_adapter();
    REQUIRE(adapter != nullptr);

    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h265_10s_1920x1080.mp4" };
    config.headless = true;
    config.dxgi_adapter = adapter;
    config.width = 1280;
    config.height = 720;
    config.use_hardware_decode = true;

    REQUIRE(renderer.initialize(config));

    auto initial = wait_for_non_black_capture(renderer);
    require_visual_frame(initial);

    renderer.seek(1000000, SeekType::Exact);
    auto seek_1s = wait_for_changed_non_black_capture(renderer, initial.hash);
    require_visual_frame(seek_1s);
    REQUIRE(seek_1s.hash != initial.hash);

    renderer.seek(3500000, SeekType::Exact);
    auto seek_3_5s = wait_for_changed_non_black_capture(renderer, seek_1s.hash);
    require_visual_frame(seek_3_5s);

    REQUIRE(seek_1s.hash != seek_3_5s.hash);

    renderer.step_forward();
    auto after_step_forward =
        wait_for_changed_non_black_capture(renderer, seek_3_5s.hash);
    require_visual_frame(after_step_forward);
    REQUIRE(after_step_forward.hash != seek_3_5s.hash);

    renderer.shutdown();
}

TEST_CASE("Renderer: headless AV1 and VP9 produce visual frames", "[renderer][hw][visual]") {
    auto* adapter = get_default_adapter();
    REQUIRE(adapter != nullptr);

    const std::vector<std::string> files = {
        video_test_dir() + "/av1_10s_1920x1080.webm",
        video_test_dir() + "/vp9_10s_1920x1080.webm",
    };

    for (const auto& file : files) {
        INFO("file=" << file);
        REQUIRE(std::filesystem::exists(file));

        Renderer renderer;
        RendererConfig config;
        config.video_paths = { file };
        config.headless = true;
        config.dxgi_adapter = adapter;
        config.width = 1280;
        config.height = 720;
        config.use_hardware_decode = true;

        REQUIRE(renderer.initialize(config));
        auto initial = wait_for_non_black_capture(renderer);
        require_visual_frame(initial);

        renderer.seek(3500000, SeekType::Exact);
        auto after_seek = wait_for_changed_non_black_capture(renderer, initial.hash);
        require_visual_frame(after_seek);
        REQUIRE(initial.hash != after_seek.hash);

        renderer.shutdown();
    }
}
