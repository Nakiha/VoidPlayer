#include <catch2/catch_test_macros.hpp>

#include "windows/player/native_player.h"
#include "test_utils.h"

using namespace vr;
using namespace vr::test;

TEST_CASE("NativePlayer: duplicate initialize preserves the active playback session",
          "[native_player]") {
    NativePlayer player;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    REQUIRE(player.initialize(config));
    REQUIRE(player.is_initialized());
    REQUIRE(player.playback().audio_output() != nullptr);
    auto* audio_output = player.playback().audio_output();

    REQUIRE_FALSE(player.initialize(config));
    REQUIRE(player.is_initialized());
    REQUIRE(player.track_count() == 1);
    REQUIRE(player.playback().audio_output() == audio_output);

    player.shutdown();
    REQUIRE_FALSE(player.is_initialized());
    REQUIRE(player.playback().audio_output() == nullptr);
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("NativePlayer: failed initialize rolls back playback and can retry",
          "[native_player]") {
    NativePlayer player;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/missing-video.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    REQUIRE_FALSE(player.initialize(config));
    REQUIRE_FALSE(player.is_initialized());
    REQUIRE(player.playback().audio_output() == nullptr);

    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    REQUIRE(player.initialize(config));
    REQUIRE(player.is_initialized());
    REQUIRE(player.playback().audio_output() != nullptr);

    player.shutdown();
    REQUIRE_FALSE(player.is_initialized());
    REQUIRE(player.playback().audio_output() == nullptr);
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("NativePlayer: facade methods fail closed outside initialized lifecycle",
          "[native_player]") {
    NativePlayer player;

    player.set_frame_callback([]() {});
    player.set_event_callback([](const RendererEvent&) {});
    player.play();
    player.pause();
    player.seek(1000);
    player.set_speed(2.0);
    player.set_loop_range(true, 0, 1000);
    player.set_audible_track(0);
    player.step_forward();
    player.step_backward();
    player.apply_layout(LayoutState{});
    player.set_background_color(0.1f, 0.2f, 0.3f, 1.0f);
    player.resize(320, 180);
    player.remove_track(0);
    player.set_track_offset(0, 1000);
    player.release_shared_texture(0, 1);

    REQUIRE_FALSE(player.is_initialized());
    REQUIRE_FALSE(player.is_playing());
    REQUIRE(player.current_pts_us() == 0);
    REQUIRE(player.current_speed() == 1.0);
    REQUIRE(player.track_count() == 0);
    REQUIRE(player.duration_us() == 0);
    REQUIRE(player.audible_track() == -1);
    REQUIRE(player.add_track(video_test_dir() + "/h264_9s_1920x1080.mp4", false) == -1);
    REQUIRE_FALSE(player.has_track(0));
    const auto dimensions = player.track_dimensions(0);
    REQUIRE(dimensions.first == 0);
    REQUIRE(dimensions.second == 0);
    REQUIRE(player.track_infos().empty());
    REQUIRE(player.track_perf_stats().empty());
    REQUIRE(player.gpu_memory_stats().total_estimated_bytes == 0);
    REQUIRE_FALSE(player.d3d_device_lost());
    REQUIRE(player.d3d_device_removed_reason() == 0);
    REQUIRE(player.texture_width() == 0);
    REQUIRE(player.texture_height() == 0);

    SharedTextureSnapshot snapshot;
    snapshot.width = 1;
    snapshot.height = 1;
    REQUIRE_FALSE(player.acquire_shared_texture(snapshot));
    REQUIRE(snapshot.type == SharedTextureHandleType::None);
    REQUIRE(snapshot.width == 0);
    REQUIRE(snapshot.height == 0);

    std::vector<uint8_t> bgra = {1, 2, 3, 4};
    int width = -1;
    int height = -1;
    REQUIRE_FALSE(player.capture_front_buffer(bgra, width, height));
    REQUIRE(bgra.empty());
    REQUIRE(width == 0);
    REQUIRE(height == 0);

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    REQUIRE(player.initialize(config));
    player.shutdown();

    player.play();
    player.seek(1000);
    player.release_shared_texture(0, 1);
    REQUIRE_FALSE(player.is_initialized());
    REQUIRE(player.track_count() == 0);
    REQUIRE(player.current_speed() == 1.0);

    destroy_window(static_cast<HWND>(config.hwnd));
}
