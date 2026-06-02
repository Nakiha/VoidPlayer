#include "renderer/overlay/analysis_overlay_renderer.h"

#include "renderer/layout/layout_geometry.h"
#include "renderer/overlay/analysis_overlay_primitives.h"
#include "renderer/render/shader_constants.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vr {
namespace {

struct TargetRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

void blend_target_pixel(uint8_t* target,
                        int width,
                        int height,
                        size_t stride,
                        int x,
                        int y,
                        analysis::OverlayColor color) {
    if (!target || x < 0 || y < 0 || x >= width || y >= height || color.a == 0) {
        return;
    }
    auto* dst = target + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
    const uint32_t src_a = color.a;
    const uint32_t inv_a = 255u - src_a;
    dst[0] = static_cast<uint8_t>((color.b * src_a + dst[0] * inv_a) / 255u);
    dst[1] = static_cast<uint8_t>((color.g * src_a + dst[1] * inv_a) / 255u);
    dst[2] = static_cast<uint8_t>((color.r * src_a + dst[2] * inv_a) / 255u);
    dst[3] = 255;
}

void fill_target_rect(uint8_t* target,
                      int width,
                      int height,
                      size_t stride,
                      TargetRect rect,
                      analysis::OverlayColor color) {
    rect.x0 = std::clamp(rect.x0, 0, width);
    rect.x1 = std::clamp(rect.x1, 0, width);
    rect.y0 = std::clamp(rect.y0, 0, height);
    rect.y1 = std::clamp(rect.y1, 0, height);
    if (rect.x0 >= rect.x1 || rect.y0 >= rect.y1 || color.a == 0) {
        return;
    }
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            blend_target_pixel(target, width, height, stride, x, y, color);
        }
    }
}

void draw_contrast_line_rect(uint8_t* target,
                             int width,
                             int height,
                             size_t stride,
                             TargetRect center,
                             analysis::OverlayColor center_color,
                             analysis::OverlayColor halo_color) {
    TargetRect halo{
        center.x0 - 1,
        center.y0 - 1,
        center.x1 + 1,
        center.y1 + 1,
    };
    fill_target_rect(target, width, height, stride, halo, halo_color);
    fill_target_rect(target, width, height, stride, center, center_color);
}

void stroke_target_rect_contrast(uint8_t* target,
                                 int width,
                                 int height,
                                 size_t stride,
                                 TargetRect rect,
                                 bool draw_right,
                                 bool draw_bottom,
                                 uint8_t line_alpha) {
    if (line_alpha == 0) {
        return;
    }
    rect.x0 = std::clamp(rect.x0, 0, width);
    rect.x1 = std::clamp(rect.x1, 0, width);
    rect.y0 = std::clamp(rect.y0, 0, height);
    rect.y1 = std::clamp(rect.y1, 0, height);
    if (rect.x0 >= rect.x1 || rect.y0 >= rect.y1) {
        return;
    }

    const analysis::OverlayColor center_color{255, 255, 255, 115};
    const analysis::OverlayColor halo_color{0, 0, 0, 166};
    const int line_width = 2;

    draw_contrast_line_rect(
        target,
        width,
        height,
        stride,
        TargetRect{rect.x0, rect.y0, rect.x1, rect.y0 + line_width},
        center_color,
        halo_color);
    draw_contrast_line_rect(
        target,
        width,
        height,
        stride,
        TargetRect{rect.x0, rect.y0, rect.x0 + line_width, rect.y1},
        center_color,
        halo_color);
    if (draw_right) {
        draw_contrast_line_rect(
            target,
            width,
            height,
            stride,
            TargetRect{rect.x1 - line_width, rect.y0, rect.x1, rect.y1},
            center_color,
            halo_color);
    }
    if (draw_bottom) {
        draw_contrast_line_rect(
            target,
            width,
            height,
            stride,
            TargetRect{rect.x0, rect.y1 - line_width, rect.x1, rect.y1},
            center_color,
            halo_color);
    }
}

void draw_target_line(uint8_t* target,
                      int width,
                      int height,
                      size_t stride,
                      int x0,
                      int y0,
                      int x1,
                      int y1,
                      analysis::OverlayColor color) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        blend_target_pixel(target, width, height, stride, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

int active_display_count(const ShaderConstants& constants) {
    return std::max(constants.track_count, 1);
}

int display_slot_for_track(const ShaderConstants& constants, int track_slot) {
    const int count = active_display_count(constants);
    for (int i = 0; i < count && i < 4; ++i) {
        if (constants.order[i] == track_slot) {
            return i;
        }
    }
    return -1;
}

bool video_point_to_target(const ShaderConstants& constants,
                           int track_slot,
                           int target_width,
                           int target_height,
                           float video_u,
                           float video_v,
                           float& out_x,
                           float& out_y) {
    if (track_slot < 0 || track_slot >= 4 ||
        constants.inv_display_size_x[track_slot] == 0.0f ||
        constants.inv_display_size_y[track_slot] == 0.0f) {
        return false;
    }
    const float local_x =
        constants.display_offset_x[track_slot] +
        (video_u + constants.view_offset_uv_x[track_slot]) /
            constants.inv_display_size_x[track_slot];
    const float local_y =
        constants.display_offset_y[track_slot] +
        (video_v + constants.view_offset_uv_y[track_slot]) /
            constants.inv_display_size_y[track_slot];
    if (!std::isfinite(local_x) || !std::isfinite(local_y)) {
        return false;
    }

    if (constants.mode == LAYOUT_SPLIT_SCREEN) {
        out_x = local_x * static_cast<float>(target_width);
        out_y = local_y * static_cast<float>(target_height);
        return true;
    }

    const int display_slot = display_slot_for_track(constants, track_slot);
    if (display_slot < 0) {
        return false;
    }
    const int count = active_display_count(constants);
    out_x = (static_cast<float>(display_slot) + local_x) *
            static_cast<float>(target_width) / static_cast<float>(count);
    out_y = local_y * static_cast<float>(target_height);
    return true;
}

bool video_rect_to_target(const ShaderConstants& constants,
                          int track_slot,
                          int target_width,
                          int target_height,
                          int video_width,
                          int video_height,
                          int x0,
                          int y0,
                          int x1,
                          int y1,
                          TargetRect& out) {
    if (video_width <= 0 || video_height <= 0) {
        return false;
    }
    float tx0 = 0.0f;
    float ty0 = 0.0f;
    float tx1 = 0.0f;
    float ty1 = 0.0f;
    if (!video_point_to_target(constants,
                               track_slot,
                               target_width,
                               target_height,
                               static_cast<float>(x0) / static_cast<float>(video_width),
                               static_cast<float>(y0) / static_cast<float>(video_height),
                               tx0,
                               ty0) ||
        !video_point_to_target(constants,
                               track_slot,
                               target_width,
                               target_height,
                               static_cast<float>(x1) / static_cast<float>(video_width),
                               static_cast<float>(y1) / static_cast<float>(video_height),
                               tx1,
                               ty1)) {
        return false;
    }
    out.x0 = static_cast<int>(std::floor(std::min(tx0, tx1)));
    out.y0 = static_cast<int>(std::floor(std::min(ty0, ty1)));
    out.x1 = static_cast<int>(std::ceil(std::max(tx0, tx1)));
    out.y1 = static_cast<int>(std::ceil(std::max(ty0, ty1)));

    if (constants.mode == LAYOUT_SPLIT_SCREEN) {
        const int split_x = static_cast<int>(
            std::lround(constants.split_pos * static_cast<float>(target_width)));
        if (constants.order[0] == track_slot) {
            out.x0 = std::max(out.x0, 0);
            out.x1 = std::min(out.x1, split_x);
        } else if (constants.order[1] == track_slot) {
            out.x0 = std::max(out.x0, split_x);
            out.x1 = std::min(out.x1, target_width);
        } else {
            return false;
        }
    } else {
        const int display_slot = display_slot_for_track(constants, track_slot);
        if (display_slot < 0) {
            return false;
        }
        const int count = active_display_count(constants);
        const int slot_x0 = target_width * display_slot / count;
        const int slot_x1 = target_width * (display_slot + 1) / count;
        out.x0 = std::max(out.x0, slot_x0);
        out.x1 = std::min(out.x1, slot_x1);
    }
    out.y0 = std::max(out.y0, 0);
    out.y1 = std::min(out.y1, target_height);
    return out.x0 < out.x1 && out.y0 < out.y1;
}

} // namespace

uint32_t pack_overlay_uv16(int a, int a_extent, int b, int b_extent) {
    auto pack_one = [](int value, int extent) -> uint32_t {
        if (extent <= 0) {
            return 0;
        }
        const int clamped = std::clamp(value, 0, extent);
        return static_cast<uint32_t>(
            std::lround(static_cast<double>(clamped) * 65535.0 / static_cast<double>(extent)));
    };
    return pack_one(a, a_extent) | (pack_one(b, b_extent) << 16);
}

AnalysisOverlayMemoryStats snapshot_analysis_overlay_memory_stats(
    const D3D11RenderResources&) {
    return {};
}

void AnalysisOverlayRenderer::reset() {
    for (auto& pixels : overlay_pixels_) {
        pixels.clear();
    }
    for (auto& rects : overlay_rects_) {
        rects.clear();
    }
    for (auto& cache : overlay_cache_) {
        cache = {};
    }
}

void AnalysisOverlayRenderer::draw(const RendererDrawSnapshot&,
                                   D3D11Device&,
                                   D3D11RenderResources&,
                                   int,
                                   int) {}

bool AnalysisOverlayRenderer::composite_bgra(const RendererDrawSnapshot& snapshot,
                                             uint8_t* target_bgra,
                                             int target_width,
                                             int target_height,
                                             size_t target_stride_bytes) {
    if (!target_bgra ||
        target_width <= 0 ||
        target_height <= 0 ||
        target_stride_bytes < static_cast<size_t>(target_width) * 4u) {
        return false;
    }

    const auto package = build_analysis_overlay_primitives(snapshot);
    if (package.empty()) {
        return false;
    }

    ShaderConstants constants = {};
    populate_layout_shader_constants(
        constants, snapshot.layout, snapshot.track_geometry, target_width, target_height);

    bool drew = false;

    for (const auto& track : package.tracks) {
        const int slot = track.slot;
        const int video_w = track.video_width;
        const int video_h = track.video_height;
        if (video_w <= 0 || video_h <= 0) {
            continue;
        }

        for (const auto& primitive : track.fill_rects) {
            TargetRect rect;
            if (!video_rect_to_target(constants,
                                      slot,
                                      target_width,
                                      target_height,
                                      video_w,
                                      video_h,
                                      primitive.x0,
                                      primitive.y0,
                                      primitive.x1,
                                      primitive.y1,
                                      rect)) {
                continue;
            }
            fill_target_rect(
                target_bgra, target_width, target_height, target_stride_bytes, rect,
                primitive.color);
            drew = true;
        }

        for (const auto& primitive : track.outline_rects) {
            TargetRect rect;
            if (!video_rect_to_target(constants,
                                      slot,
                                      target_width,
                                      target_height,
                                      video_w,
                                      video_h,
                                      primitive.x0,
                                      primitive.y0,
                                      primitive.x1,
                                      primitive.y1,
                                      rect)) {
                continue;
            }
            stroke_target_rect_contrast(target_bgra,
                                        target_width,
                                        target_height,
                                        target_stride_bytes,
                                        rect,
                                        primitive.x1 >= video_w,
                                        primitive.y1 >= video_h,
                                        track.line_alpha);
            drew = true;
        }

        for (const auto& line : track.motion_lines) {
            float x0 = 0.0f;
            float y0 = 0.0f;
            float x1 = 0.0f;
            float y1 = 0.0f;
            if (!video_point_to_target(constants,
                                       slot,
                                       target_width,
                                       target_height,
                                       static_cast<float>(line.x0) /
                                           static_cast<float>(video_w),
                                       static_cast<float>(line.y0) /
                                           static_cast<float>(video_h),
                                       x0,
                                       y0) ||
                !video_point_to_target(constants,
                                       slot,
                                       target_width,
                                       target_height,
                                       static_cast<float>(line.x1) /
                                           static_cast<float>(video_w),
                                       static_cast<float>(line.y1) /
                                           static_cast<float>(video_h),
                                       x1,
                                       y1)) {
                continue;
            }
            draw_target_line(
                target_bgra,
                target_width,
                target_height,
                target_stride_bytes,
                static_cast<int>(std::lround(x0)),
                static_cast<int>(std::lround(y0)),
                static_cast<int>(std::lround(x1)),
                static_cast<int>(std::lround(y1)),
                line.color);
            drew = true;
        }
    }

    return drew;
}

} // namespace vr
