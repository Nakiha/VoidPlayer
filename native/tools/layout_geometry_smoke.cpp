#include "renderer/layout/layout_geometry.h"

#include <cmath>
#include <iostream>

namespace {

bool near(float lhs, float rhs, float tolerance = 0.0001f) {
    return std::fabs(lhs - rhs) <= tolerance;
}

bool finite(float value) {
    return std::isfinite(value);
}

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << "\n";
    }
    return condition;
}

vr::LayoutTrackGeometryList two_wide_tracks() {
    vr::LayoutTrackGeometryList tracks = {};
    tracks[0] = {true, 1920, 1080, 16.0f / 9.0f};
    tracks[1] = {true, 1280, 720, 16.0f / 9.0f};
    return tracks;
}

bool side_by_side_uniform_pixels() {
    vr::LayoutState layout;
    layout.mode = vr::LAYOUT_SIDE_BY_SIDE;
    layout.pixel_size_mode = vr::PIXEL_SIZE_UNIFORM_VIDEO_PIXELS;
    layout.order[0] = 0;
    layout.order[1] = 1;

    const auto tracks = two_wide_tracks();
    vr::ShaderConstants constants = {};
    vr::populate_layout_shader_constants(constants, layout, tracks, 2000, 1000);

    bool ok = true;
    ok &= check(constants.track_count == 2, "expected two active layout tracks");
    ok &= check(constants.order[0] == 0 && constants.order[1] == 1,
                "layout order was not preserved");
    ok &= check(near(constants.track_scale[0], 1.0f),
                "primary track should keep reference pixel scale");
    ok &= check(near(constants.track_scale[1], 2.0f / 3.0f),
                "secondary track should preserve uniform video-pixel scale");
    ok &= check(near(constants.display_offset_x[0], 0.0f),
                "side-by-side primary x offset should be centered");
    ok &= check(near(constants.display_offset_y[0], 0.21875f),
                "side-by-side primary y offset should letterbox");
    ok &= check(near(constants.inv_display_size_x[0], 1.0f),
                "side-by-side primary inverse width is wrong");
    ok &= check(near(constants.inv_display_size_y[0], 16.0f / 9.0f),
                "side-by-side primary inverse height is wrong");

    const auto display = vr::display_pixel_size_for_layout(2000, 1000, layout, tracks);
    ok &= check(near(display.first, 1000.0f), "display width should match one slot");
    ok &= check(near(display.second, 562.5f), "display height should preserve aspect");
    return ok;
}

bool split_zoom_pan_uv_offsets() {
    vr::LayoutState layout;
    layout.mode = vr::LAYOUT_SPLIT_SCREEN;
    layout.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
    layout.zoom_ratio = 2.0f;
    layout.view_offset[0] = 120.0f;
    layout.view_offset[1] = -40.0f;

    vr::LayoutTrackGeometryList tracks = {};
    tracks[0] = {true, 1920, 1080, 16.0f / 9.0f};

    vr::ShaderConstants constants = {};
    vr::populate_layout_shader_constants(constants, layout, tracks, 1600, 900);

    bool ok = true;
    ok &= check(constants.track_count == 1, "split layout should expose one active track");
    ok &= check(near(constants.display_offset_x[0], -0.5f),
                "zoomed split x offset should expand around center");
    ok &= check(near(constants.display_offset_y[0], -0.5f),
                "zoomed split y offset should expand around center");
    ok &= check(near(constants.inv_display_size_x[0], 0.5f),
                "zoomed split inverse width should halve");
    ok &= check(near(constants.inv_display_size_y[0], 0.5f),
                "zoomed split inverse height should halve");
    ok &= check(near(constants.view_offset_uv_x[0], 120.0f / 3200.0f),
                "pan x should be normalized by zoomed display width");
    ok &= check(near(constants.view_offset_uv_y[0], -40.0f / 1800.0f),
                "pan y should be normalized by zoomed display height");
    return ok;
}

bool resize_preserves_visible_content_position() {
    vr::LayoutState layout;
    layout.mode = vr::LAYOUT_SIDE_BY_SIDE;
    layout.pixel_size_mode = vr::PIXEL_SIZE_UNIFORM_VIDEO_PIXELS;
    layout.view_offset[0] = 100.0f;
    layout.view_offset[1] = -50.0f;
    layout.order[0] = 0;
    layout.order[1] = 1;

    const auto tracks = two_wide_tracks();
    const auto adjustment = vr::adjust_layout_view_offset_for_resize(
        layout, 2000, 1000, 4000, 2000, tracks);

    bool ok = true;
    ok &= check(adjustment.adjusted_x && adjustment.adjusted_y,
                "resize should adjust both pan axes");
    ok &= check(near(adjustment.old_offset_x, 100.0f) &&
                    near(adjustment.old_offset_y, -50.0f),
                "resize adjustment should report old offsets");
    ok &= check(near(layout.view_offset[0], 200.0f) &&
                    near(layout.view_offset[1], -100.0f),
                "resize adjustment should scale offsets with display pixels");
    ok &= check(near(adjustment.new_offset_x, 200.0f) &&
                    near(adjustment.new_offset_y, -100.0f),
                "resize adjustment should report new offsets");
    return ok;
}

bool invalid_track_aspect_has_deterministic_fallback() {
    vr::LayoutState layout;
    layout.mode = vr::LAYOUT_SPLIT_SCREEN;
    layout.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;

    vr::LayoutTrackGeometryList tracks = {};
    tracks[0] = {true, 0, 0, 0.0f};

    vr::ShaderConstants constants = {};
    vr::populate_layout_shader_constants(constants, layout, tracks, 1280, 720);

    bool ok = true;
    ok &= check(constants.track_count == 1, "invalid geometry track should remain active");
    ok &= check(finite(constants.display_offset_x[0]) &&
                    finite(constants.display_offset_y[0]) &&
                    finite(constants.inv_display_size_x[0]) &&
                    finite(constants.inv_display_size_y[0]),
                "invalid geometry fallback should stay finite");

    const auto display = vr::display_pixel_size_for_layout(1280, 720, layout, tracks);
    ok &= check(near(display.first, 1280.0f) && near(display.second, 720.0f),
                "invalid aspect should fall back to slot aspect");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= side_by_side_uniform_pixels();
    ok &= split_zoom_pan_uv_offsets();
    ok &= resize_preserves_visible_content_position();
    ok &= invalid_track_aspect_has_deterministic_fallback();
    return ok ? 0 : 1;
}
