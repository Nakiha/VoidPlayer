#include <catch2/catch_test_macros.hpp>

#include "video_renderer/layout_validation.h"
#include "video_renderer/layout_controller.h"
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

TEST_CASE("Layout validation enforces viewport guardrails",
          "[renderer_config][layout]") {
    LayoutState layout;
    layout.order[0] = 1;
    layout.order[1] = 2;
    layout.order[2] = -1;
    layout.order[3] = -1;
    REQUIRE(validate_layout_state(layout).ok);

    auto invalid = layout;
    invalid.split_pos = -0.01f;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);

    invalid = layout;
    invalid.split_pos = 1.01f;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);

    invalid = layout;
    invalid.zoom_ratio = kMinLayoutZoomRatio - 0.01f;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);

    invalid = layout;
    invalid.zoom_ratio = kMaxLayoutZoomRatio + 0.01f;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);
}

TEST_CASE("Layout validation treats order entries as file IDs",
          "[renderer_config][layout]") {
    LayoutState layout;
    layout.order[0] = 42;
    layout.order[1] = 99;
    layout.order[2] = -1;
    layout.order[3] = 0;
    REQUIRE(validate_layout_state(layout).ok);

    auto invalid = layout;
    invalid.order[3] = 42;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);

    invalid = layout;
    invalid.order[2] = -2;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);
}

TEST_CASE("LayoutController owns file-id to slot order translation",
          "[renderer_config][layout]") {
    LayoutController controller;
    LayoutState layout;
    controller.reset(layout);

    controller.append_track(layout, 10, 0);
    controller.append_track(layout, 20, 2);
    REQUIRE(layout.order[0] == 0);
    REQUIRE(layout.order[1] == 2);
    REQUIRE(layout.order[2] == 0);
    REQUIRE(layout.order[3] == 0);

    auto snapshot = controller.snapshot(layout);
    REQUIRE(snapshot.order[0] == 10);
    REQUIRE(snapshot.order[1] == 20);
    REQUIRE(snapshot.order[2] == -1);
    REQUIRE(snapshot.order[3] == -1);

    LayoutState requested;
    requested.order[0] = 20;
    requested.order[1] = 10;
    requested.order[2] = 99;
    requested.order[3] = -1;
    controller.apply(layout, requested, [](int file_id) {
        if (file_id == 10) return 0;
        if (file_id == 20) return 2;
        return -1;
    });
    REQUIRE(layout.order[0] == 2);
    REQUIRE(layout.order[1] == 0);
    REQUIRE(layout.order[2] == 0);
    REQUIRE(layout.order[3] == 0);

    controller.remove_track(layout, 20, [](int file_id) {
        if (file_id == 10) return 0;
        return -1;
    });
    snapshot = controller.snapshot(layout);
    REQUIRE(snapshot.order[0] == 10);
    REQUIRE(snapshot.order[1] == 99);
    REQUIRE(snapshot.order[2] == -1);
    REQUIRE(snapshot.order[3] == -1);
    REQUIRE(layout.order[0] == 0);
    REQUIRE(layout.order[1] == 0);
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
