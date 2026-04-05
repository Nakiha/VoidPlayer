#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/renderer.h"
#include <thread>
#include <chrono>

using namespace vr;
using namespace vr::test;

TEST_CASE("Renderer: initialize with single H264 file", "[renderer]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;

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
