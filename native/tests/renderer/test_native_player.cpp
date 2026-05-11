#include <catch2/catch_test_macros.hpp>

#include "player/native_player.h"
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
