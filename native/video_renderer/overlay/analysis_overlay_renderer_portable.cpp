#include "video_renderer/overlay/analysis_overlay_renderer.h"

#include "analysis/analysis_manager.h"
#include "analysis/cache/overlay_raster.h"
#include "video_renderer/layout/layout_geometry.h"
#include "video_renderer/render/shader_constants.h"

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

uint8_t overlay_alpha(int opacity_permille, bool heatmap) {
    const int base = std::clamp(opacity_permille, 0, 1000) * 255 / 1000;
    return static_cast<uint8_t>(heatmap ? base : base * 2 / 5);
}

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

void stroke_target_rect(uint8_t* target,
                        int width,
                        int height,
                        size_t stride,
                        TargetRect rect,
                        analysis::OverlayColor color) {
    rect.x0 = std::clamp(rect.x0, 0, width - 1);
    rect.x1 = std::clamp(rect.x1, 0, width);
    rect.y0 = std::clamp(rect.y0, 0, height - 1);
    rect.y1 = std::clamp(rect.y1, 0, height);
    if (rect.x0 >= rect.x1 || rect.y0 >= rect.y1 || color.a == 0) {
        return;
    }
    const int right = rect.x1 - 1;
    const int bottom = rect.y1 - 1;
    for (int x = rect.x0; x <= right; ++x) {
        blend_target_pixel(target, width, height, stride, x, rect.y0, color);
        blend_target_pixel(target, width, height, stride, x, bottom, color);
    }
    for (int y = rect.y0; y <= bottom; ++y) {
        blend_target_pixel(target, width, height, stride, rect.x0, y, color);
        blend_target_pixel(target, width, height, stride, right, y, color);
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

void AnalysisOverlayRenderer::draw(const PresentDecision&,
                                   const RendererDrawTrackSnapshotList&,
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

    auto& manager = analysis::AnalysisManager::instance();
    const auto& overlay = manager.overlay_state();
    const bool show_grid = overlay.show_cu_grid.load(std::memory_order_acquire);
    const bool show_qp = overlay.show_qp_heatmap.load(std::memory_order_acquire);
    const bool show_pred = overlay.show_pred_mode.load(std::memory_order_acquire);
    const bool show_lines = overlay.show_pred_lines.load(std::memory_order_acquire);
    const bool show_bit_cost = overlay.show_cu_bit_cost_heatmap.load(std::memory_order_acquire);
    const int mode = overlay.mode.load(std::memory_order_acquire);
    const int file_id = overlay.track_file_id.load(std::memory_order_acquire);
    const int opacity_permille =
        std::clamp(overlay.opacity_permille.load(std::memory_order_acquire), 0, 1000);

    if (!show_grid && !show_qp && !show_pred && !show_lines && !show_bit_cost &&
        mode != 0 && mode != 1 && mode != 2 && mode != 3 && mode != 4) {
        return false;
    }

    auto overlay_tracks = manager.overlay_track_snapshot();
    if (overlay_tracks.empty() && manager.is_loaded() && file_id >= 0) {
        overlay_tracks.emplace_back(file_id, manager.session_snapshot());
    }
    if (overlay_tracks.empty()) {
        return false;
    }

    ShaderConstants constants = {};
    populate_layout_shader_constants(
        constants, snapshot.layout, snapshot.track_geometry, target_width, target_height);

    const bool qp_primary = show_qp || mode == 1 || mode == 3;
    const bool bit_cost_primary = show_bit_cost || mode == 2 || mode == 4;
    const bool pred_primary = show_pred;
    const bool heatmap_primary = qp_primary || bit_cost_primary;
    const uint8_t fill_alpha = overlay_alpha(opacity_permille, heatmap_primary);
    const uint8_t line_alpha = static_cast<uint8_t>(opacity_permille * 255 / 1000);
    bool drew = false;

    auto find_slot = [&](int track_file_id) -> int {
        for (size_t i = 0; i < snapshot.tracks.size(); ++i) {
            if (snapshot.tracks[i].active && snapshot.tracks[i].file_id == track_file_id) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    for (const auto& [track_file_id, track_analysis] : overlay_tracks) {
        if (!track_analysis) {
            continue;
        }
        const int slot = find_slot(track_file_id);
        if (slot < 0 || slot >= static_cast<int>(snapshot.tracks.size())) {
            continue;
        }
        const int frame_idx = track_analysis->current_frame_idx(
            snapshot.decision.frames[slot].has_value()
                ? snapshot.decision.frames[slot]->pts_us
                : std::max<int64_t>(
                      0, snapshot.decision.current_pts_us - snapshot.tracks[slot].offset_us));
        if (frame_idx < 0 || frame_idx >= track_analysis->frame_count()) {
            continue;
        }
        const int video_w = static_cast<int>(track_analysis->video_width());
        const int video_h = static_cast<int>(track_analysis->video_height());
        if (video_w <= 0 || video_h <= 0) {
            continue;
        }
        const auto frame = track_analysis->read_overlay_frame(frame_idx);
        if (frame.cus.empty()) {
            continue;
        }

        for (const auto& cu : frame.cus) {
            const auto& c = cu.common;
            const int x0 = std::clamp(static_cast<int>(c.x), 0, video_w);
            const int y0 = std::clamp(static_cast<int>(c.y), 0, video_h);
            const int x1 = std::clamp(static_cast<int>(c.x + c.w), 0, video_w);
            const int y1 = std::clamp(static_cast<int>(c.y + c.h), 0, video_h);
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            TargetRect rect;
            if (!video_rect_to_target(
                    constants, slot, target_width, target_height, video_w, video_h,
                    x0, y0, x1, y1, rect)) {
                continue;
            }

            if (bit_cost_primary && fill_alpha > 0) {
                fill_target_rect(
                    target_bgra,
                    target_width,
                    target_height,
                    target_stride_bytes,
                    rect,
                    analysis::cu_bit_density_color(c, fill_alpha));
                drew = true;
            } else if (qp_primary && fill_alpha > 0) {
                fill_target_rect(
                    target_bgra,
                    target_width,
                    target_height,
                    target_stride_bytes,
                    rect,
                    analysis::qp_color(c.qp, fill_alpha));
                drew = true;
            } else if (pred_primary && fill_alpha > 0) {
                fill_target_rect(
                    target_bgra,
                    target_width,
                    target_height,
                    target_stride_bytes,
                    rect,
                    c.pred_mode == 1
                        ? analysis::OverlayColor{80, 235, 90, static_cast<uint8_t>(fill_alpha * 3 / 4)}
                        : analysis::pred_color(
                              c.pred_mode, cu.inter, static_cast<uint8_t>(fill_alpha * 3 / 4)));
                drew = true;
            }

            if ((show_grid || mode == 0 || pred_primary) && line_alpha > 0) {
                stroke_target_rect(
                    target_bgra,
                    target_width,
                    target_height,
                    target_stride_bytes,
                    rect,
                    analysis::OverlayColor{255, 255, 255, line_alpha});
                drew = true;
            }

            if (show_lines && line_alpha > 0 && c.pred_mode != 1) {
                float cx = 0.0f;
                float cy = 0.0f;
                if (!video_point_to_target(constants,
                                           slot,
                                           target_width,
                                           target_height,
                                           static_cast<float>(x0 + x1) * 0.5f /
                                               static_cast<float>(video_w),
                                           static_cast<float>(y0 + y1) * 0.5f /
                                               static_cast<float>(video_h),
                                           cx,
                                           cy)) {
                    continue;
                }
                const int dx = std::clamp(static_cast<int>(cu.inter.mv_l0_x / 16), -80, 80);
                const int dy = std::clamp(static_cast<int>(cu.inter.mv_l0_y / 16), -80, 80);
                TargetRect tip;
                if (!video_rect_to_target(
                        constants,
                        slot,
                        target_width,
                        target_height,
                        video_w,
                        video_h,
                        std::clamp((x0 + x1) / 2 + dx, 0, video_w),
                        std::clamp((y0 + y1) / 2 + dy, 0, video_h),
                        std::clamp((x0 + x1) / 2 + dx + 1, 0, video_w),
                        std::clamp((y0 + y1) / 2 + dy + 1, 0, video_h),
                        tip)) {
                    continue;
                }
                draw_target_line(
                    target_bgra,
                    target_width,
                    target_height,
                    target_stride_bytes,
                    static_cast<int>(std::lround(cx)),
                    static_cast<int>(std::lround(cy)),
                    tip.x0,
                    tip.y0,
                    analysis::OverlayColor{80, 180, 255, line_alpha});
                drew = true;
            }
        }
    }

    return drew;
}

} // namespace vr
