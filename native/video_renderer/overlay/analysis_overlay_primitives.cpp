#include "video_renderer/overlay/analysis_overlay_primitives.h"

#include "analysis/analysis_manager.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <utility>

namespace vr {
namespace {

uint8_t overlay_fill_alpha(int opacity_permille, bool heatmap) {
    const int base = std::clamp(opacity_permille, 0, 1000) * 255 / 1000;
    return static_cast<uint8_t>(heatmap ? base : base * 2 / 5);
}

int find_track_slot(const RendererDrawSnapshot& snapshot, int track_file_id) {
    for (size_t i = 0; i < snapshot.tracks.size(); ++i) {
        if (snapshot.tracks[i].active && snapshot.tracks[i].file_id == track_file_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

AnalysisOverlayPrimitivePackage build_analysis_overlay_primitives(
    const RendererDrawSnapshot& snapshot) {
    AnalysisOverlayPrimitivePackage package;

    auto& manager = analysis::AnalysisManager::instance();
    const auto& overlay = manager.overlay_state();
    const bool show_grid = overlay.show_cu_grid.load(std::memory_order_acquire);
    const bool show_qp = overlay.show_qp_heatmap.load(std::memory_order_acquire);
    const bool show_pred = overlay.show_pred_mode.load(std::memory_order_acquire);
    const bool show_lines = overlay.show_pred_lines.load(std::memory_order_acquire);
    const bool show_bit_cost =
        overlay.show_cu_bit_cost_heatmap.load(std::memory_order_acquire);
    const int mode = overlay.mode.load(std::memory_order_acquire);
    const int file_id = overlay.track_file_id.load(std::memory_order_acquire);
    const int opacity_permille =
        std::clamp(overlay.opacity_permille.load(std::memory_order_acquire), 0, 1000);

    if (!show_grid && !show_qp && !show_pred && !show_lines && !show_bit_cost &&
        mode != 0 && mode != 1 && mode != 2 && mode != 3 && mode != 4) {
        return package;
    }

    auto overlay_tracks = manager.overlay_track_snapshot();
    if (overlay_tracks.empty() && manager.is_loaded() && file_id >= 0) {
        overlay_tracks.emplace_back(file_id, manager.session_snapshot());
    }
    if (overlay_tracks.empty()) {
        return package;
    }

    const bool qp_primary = show_qp || mode == 1 || mode == 3;
    const bool bit_cost_primary = show_bit_cost || mode == 2 || mode == 4;
    const bool pred_primary = show_pred;
    const bool heatmap_primary = qp_primary || bit_cost_primary;
    const uint8_t fill_alpha = overlay_fill_alpha(opacity_permille, heatmap_primary);
    const uint8_t line_alpha =
        static_cast<uint8_t>(std::clamp(opacity_permille * 255 / 1000, 0, 255));
    const bool needs_outlines = line_alpha > 0 && (show_grid || mode == 0 || pred_primary);

    for (const auto& [track_file_id, track_analysis] : overlay_tracks) {
        if (!track_analysis) {
            continue;
        }
        const int slot = find_track_slot(snapshot, track_file_id);
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
        // TODO(analysis-overlay): cache per-frame primitive packages during
        // the planned overlay redesign. The presentation path must stay free
        // of filesystem scans and heavy cache refresh work.
        const auto frame = track_analysis->read_overlay_frame(frame_idx);
        if (frame.cus.empty()) {
            continue;
        }

        AnalysisOverlayTrackPrimitives track;
        track.slot = slot;
        track.track_file_id = track_file_id;
        track.frame_index = frame_idx;
        track.video_width = video_w;
        track.video_height = video_h;
        track.mode = mode;
        track.opacity_permille = opacity_permille;
        track.show_grid = show_grid;
        track.show_qp = show_qp;
        track.show_pred = show_pred;
        track.show_lines = show_lines;
        track.show_bit_cost = show_bit_cost;
        track.line_alpha = line_alpha;
        track.fill_rects.reserve(frame.cus.size());
        track.outline_rects.reserve(frame.cus.size());
        track.motion_lines.reserve(show_lines ? frame.cus.size() : 0);

        for (const auto& cu : frame.cus) {
            const auto& c = cu.common;
            const int x0 = std::clamp(static_cast<int>(c.x), 0, video_w);
            const int y0 = std::clamp(static_cast<int>(c.y), 0, video_h);
            const int x1 = std::clamp(static_cast<int>(c.x + c.w), 0, video_w);
            const int y1 = std::clamp(static_cast<int>(c.y + c.h), 0, video_h);
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }

            if (bit_cost_primary && fill_alpha > 0) {
                track.fill_rects.push_back(
                    {x0, y0, x1, y1, analysis::cu_bit_density_color(c, fill_alpha)});
            } else if (qp_primary && fill_alpha > 0) {
                track.fill_rects.push_back({x0, y0, x1, y1, analysis::qp_color(c.qp, fill_alpha)});
            } else if (pred_primary && fill_alpha > 0) {
                track.fill_rects.push_back(
                    {x0,
                     y0,
                     x1,
                     y1,
                     c.pred_mode == 1
                         ? analysis::OverlayColor{80, 235, 90,
                                                   static_cast<uint8_t>(fill_alpha * 3 / 4)}
                         : analysis::pred_color(
                               c.pred_mode, cu.inter,
                               static_cast<uint8_t>(fill_alpha * 3 / 4))});
            }

            if (needs_outlines) {
                track.outline_rects.push_back(
                    {x0, y0, x1, y1, analysis::OverlayColor{255, 255, 255, line_alpha}});
            }

            if (show_lines && line_alpha > 0 && c.pred_mode != 1) {
                const int cx = (x0 + x1) / 2;
                const int cy = (y0 + y1) / 2;
                const int dx = std::clamp(static_cast<int>(cu.inter.mv_l0_x / 16), -80, 80);
                const int dy = std::clamp(static_cast<int>(cu.inter.mv_l0_y / 16), -80, 80);
                track.motion_lines.push_back(
                    {cx,
                     cy,
                     std::clamp(cx + dx, 0, video_w),
                     std::clamp(cy + dy, 0, video_h),
                     analysis::OverlayColor{80, 180, 255, line_alpha}});
            }
        }

        if (!track.fill_rects.empty() ||
            !track.outline_rects.empty() ||
            !track.motion_lines.empty()) {
            package.tracks.push_back(std::move(track));
        }
    }

    return package;
}

} // namespace vr
