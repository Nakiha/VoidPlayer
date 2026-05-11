#include <catch2/catch_test_macros.hpp>

#include "video_renderer/renderer_config_validation.h"

using namespace vr;

namespace {

RendererConfig valid_windowed_config() {
    RendererConfig config;
    config.video_paths = {"C:/video.mp4"};
    config.hwnd = reinterpret_cast<void*>(0x1234);
    config.width = 1920;
    config.height = 1080;
    config.use_hardware_decode = true;
    return config;
}

} // namespace

TEST_CASE("Renderer config validation accepts valid windowed config",
          "[renderer_config]") {
    const auto result = validate_renderer_config(valid_windowed_config());
    REQUIRE(result.ok);
}

TEST_CASE("Renderer config validation rejects invalid dimensions",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.width = 0;
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.height = kMaxRendererDimension + 1;
    REQUIRE_FALSE(validate_renderer_config(config).ok);
}

TEST_CASE("Renderer config validation rejects invalid path lists",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.video_paths.clear();
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.video_paths = {""};
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.video_paths.assign(kMaxRendererVideoPaths + 1, "C:/video.mp4");
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.video_paths = {std::string("\xC3\x28", 2)};
    REQUIRE_FALSE(validate_renderer_config(config).ok);
}

TEST_CASE("Renderer config validation enforces headless interop shape",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.headless = true;
    config.backend.type = RendererBackendType::D3D11;
    config.backend.adapter = nullptr;
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config.hwnd = nullptr;
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config.backend.adapter = reinterpret_cast<void*>(0x5678);
    REQUIRE(validate_renderer_config(config).ok);
}

TEST_CASE("Renderer config validation covers speed and loop range",
          "[renderer_config]") {
    REQUIRE(validate_playback_speed(1.0).ok);
    REQUIRE(validate_playback_speed(kMaxPlaybackSpeed).ok);
    REQUIRE_FALSE(validate_playback_speed(0.0).ok);
    REQUIRE_FALSE(validate_playback_speed(kMaxPlaybackSpeed + 0.1).ok);

    REQUIRE(validate_loop_range(false, 0, 0).ok);
    REQUIRE(validate_loop_range(true, 0, 1000).ok);
    REQUIRE_FALSE(validate_loop_range(true, -1, 1000).ok);
    REQUIRE_FALSE(validate_loop_range(true, 1000, 1000).ok);
}

TEST_CASE("Native resource budget exposes renderer guardrails",
          "[renderer_config]") {
    constexpr auto budget = default_native_resource_budget();
    REQUIRE(budget.max_tracks == kMaxRendererVideoPaths);
    REQUIRE(budget.max_dimension == kMaxRendererDimension);
    REQUIRE(budget.max_path_bytes == kMaxRendererPathBytes);
    REQUIRE(budget.max_cpu_frame_bytes == kMaxCpuFrameBytes);
    REQUIRE(budget.max_capture_frame_bytes == kMaxCaptureFrameBytes);
    REQUIRE(budget.max_exact_seek_reorder_frames == kMaxExactSeekReorderFrames);
    REQUIRE(budget.high_resolution_track_pixels == kHighResolutionTrackPixels);
    REQUIRE(budget.default_track_forward_depth == kDefaultTrackForwardDepth);
    REQUIRE(budget.default_track_backward_depth == kDefaultTrackBackwardDepth);
    REQUIRE(budget.high_resolution_hardware_track_forward_depth ==
            kHighResolutionHardwareTrackForwardDepth);
    REQUIRE(budget.max_playback_speed == kMaxPlaybackSpeed);
}
