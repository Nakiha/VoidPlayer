#include <catch2/catch_test_macros.hpp>

#include "video_renderer/layout_validation.h"
#include "video_renderer/layout_controller.h"
#include "video_renderer/layout_geometry.h"
#include "video_renderer/render_loop_controller.h"
#include "video_renderer/renderer_config_validation.h"

#include <chrono>
#include <cmath>

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

bool layout_float_near(float lhs, float rhs, float epsilon = 0.0001f) {
    return std::fabs(lhs - rhs) <= epsilon;
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

TEST_CASE("Layout geometry computes shader constants outside Renderer",
          "[renderer_config][layout]") {
    LayoutState layout;
    layout.mode = LAYOUT_SIDE_BY_SIDE;
    layout.zoom_ratio = 1.0f;
    layout.pixel_size_mode = PIXEL_SIZE_UNIFORM_VIDEO_PIXELS;
    layout.order[0] = 0;
    layout.order[1] = 1;
    layout.order[2] = 0;
    layout.order[3] = 0;

    LayoutTrackGeometryList tracks = {};
    tracks[0] = {true, 1920, 1080, 16.0f / 9.0f};
    tracks[1] = {true, 1280, 720, 16.0f / 9.0f};

    ShaderConstants constants = {};
    populate_layout_shader_constants(constants, layout, tracks, 2000, 1000);

    REQUIRE(constants.track_count == 2);
    REQUIRE(constants.order[0] == 0);
    REQUIRE(constants.order[1] == 1);
    REQUIRE(layout_float_near(constants.track_scale[0], 1.0f));
    REQUIRE(layout_float_near(constants.track_scale[1], 2.0f / 3.0f));
    REQUIRE(layout_float_near(constants.display_offset_x[0], 0.0f));
    REQUIRE(layout_float_near(constants.display_offset_y[0], 0.21875f));
    REQUIRE(layout_float_near(constants.inv_display_size_x[0], 1.0f));
    REQUIRE(layout_float_near(constants.inv_display_size_y[0], 16.0f / 9.0f));

    const auto display = display_pixel_size_for_layout(2000, 1000, layout, tracks);
    REQUIRE(layout_float_near(display.first, 1000.0f));
    REQUIRE(layout_float_near(display.second, 562.5f));
}

TEST_CASE("Layout geometry owns resize view offset scaling",
          "[renderer_config][layout]") {
    LayoutState layout;
    layout.mode = LAYOUT_SIDE_BY_SIDE;
    layout.zoom_ratio = 1.0f;
    layout.pixel_size_mode = PIXEL_SIZE_UNIFORM_VIDEO_PIXELS;
    layout.view_offset[0] = 100.0f;
    layout.view_offset[1] = -50.0f;
    layout.order[0] = 0;
    layout.order[1] = 1;

    LayoutTrackGeometryList tracks = {};
    tracks[0] = {true, 1920, 1080, 16.0f / 9.0f};
    tracks[1] = {true, 1280, 720, 16.0f / 9.0f};

    const auto adjustment = adjust_layout_view_offset_for_resize(
        layout, 2000, 1000, 4000, 2000, tracks);
    REQUIRE(adjustment.adjusted_x);
    REQUIRE(adjustment.adjusted_y);
    REQUIRE(layout_float_near(adjustment.old_offset_x, 100.0f));
    REQUIRE(layout_float_near(adjustment.old_offset_y, -50.0f));
    REQUIRE(layout_float_near(layout.view_offset[0], 200.0f));
    REQUIRE(layout_float_near(layout.view_offset[1], -100.0f));
    REQUIRE(layout_float_near(adjustment.new_offset_x, 200.0f));
    REQUIRE(layout_float_near(adjustment.new_offset_y, -100.0f));

    layout.view_offset[0] = 12.0f;
    layout.view_offset[1] = 34.0f;
    const auto skipped = adjust_layout_view_offset_for_resize(
        layout, 0, 1000, 4000, 2000, tracks);
    REQUIRE_FALSE(skipped.adjusted_x);
    REQUIRE_FALSE(skipped.adjusted_y);
    REQUIRE(layout_float_near(layout.view_offset[0], 12.0f));
    REQUIRE(layout_float_near(layout.view_offset[1], 34.0f));
}

TEST_CASE("RenderLoopController owns loop timing policy",
          "[renderer_config][render_loop]") {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    RenderLoopController controller;
    const auto t0 = Clock::time_point{};

    REQUIRE(controller.should_apply_resize(t0 + 34ms));
    controller.mark_resize_applied(t0 + 34ms);
    REQUIRE_FALSE(controller.should_apply_resize(t0 + 50ms));
    REQUIRE(controller.should_apply_resize(t0 + 67ms));

    controller.start(t0);
    int64_t pts_delta = -1;
    REQUIRE_FALSE(controller.should_emit_diagnostics(t0 + 1999ms, 5000, pts_delta));
    REQUIRE(controller.should_emit_diagnostics(t0 + 2000ms, 5000, pts_delta));
    REQUIRE(pts_delta == 5000);
    REQUIRE(controller.should_emit_diagnostics(t0 + 4000ms, 9000, pts_delta));
    REQUIRE(pts_delta == 4000);

    REQUIRE(controller.frame_deadline_sleep(1000, 9000, 1.0, 8000).count() == 8000);
    REQUIRE(controller.frame_deadline_sleep(1000, 30000, 1.0, 8000).count() == 8000);
    REQUIRE(controller.frame_deadline_sleep(1000, 5000, 2.0, 8000).count() == 2000);
    REQUIRE(controller.frame_deadline_sleep(5000, 1000, 1.0, 8000).count() == 0);
    REQUIRE(controller.frame_deadline_sleep(1000, 5000, 0.0, 8000).count() == 0);
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
