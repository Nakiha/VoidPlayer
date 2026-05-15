#include "video_renderer/layout_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace vr {
namespace {

int active_track_count(const LayoutTrackGeometryList& tracks) {
    int count = 0;
    for (const auto& track : tracks) {
        if (track.active) {
            ++count;
        }
    }
    return count;
}

float slot_width_for_layout(int width, const LayoutState& layout, int active_count) {
    float slot_w = static_cast<float>(width);
    if (layout.mode != LAYOUT_SPLIT_SCREEN && active_count > 1) {
        slot_w /= static_cast<float>(active_count);
    }
    return slot_w;
}

int largest_active_track(const LayoutTrackGeometryList& tracks) {
    int ref_idx = -1;
    int max_pixels = 0;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        if (!tracks[i].active) {
            continue;
        }
        const int pixels = tracks[i].width * tracks[i].height;
        if (pixels > max_pixels) {
            max_pixels = pixels;
            ref_idx = i;
        }
    }
    return ref_idx;
}

float uniform_pixel_scale_for_track(const LayoutTrackGeometryList& tracks,
                                    int track_idx,
                                    float slot_w,
                                    float slot_h) {
    const int ref_idx = largest_active_track(tracks);
    if (ref_idx < 0 || !tracks[track_idx].active) {
        return 1.0f;
    }

    float ref_density = 1.0f;
    const float ref_w = static_cast<float>(tracks[ref_idx].width);
    const float ref_h = static_cast<float>(tracks[ref_idx].height);
    if (ref_w > 0.0f && ref_h > 0.0f) {
        ref_density = std::min(slot_w / ref_w, slot_h / ref_h);
    }

    float track_density = 1.0f;
    const float track_w = static_cast<float>(tracks[track_idx].width);
    const float track_h = static_cast<float>(tracks[track_idx].height);
    if (track_w > 0.0f && track_h > 0.0f) {
        track_density = std::min(slot_w / track_w, slot_h / track_h);
    }
    return (track_density > 0.0f) ? ref_density / track_density : 1.0f;
}

int first_display_track(const LayoutState& layout, const LayoutTrackGeometryList& tracks) {
    for (int display = 0; display < 4; ++display) {
        const int candidate = layout.order[display];
        if (candidate >= 0 &&
            candidate < static_cast<int>(tracks.size()) &&
            tracks[candidate].active) {
            return candidate;
        }
    }
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        if (tracks[i].active) {
            return i;
        }
    }
    return -1;
}

} // namespace

LayoutTrackGeometryList snapshot_layout_track_geometry(
    const TrackPipelineManager& tracks) {
    LayoutTrackGeometryList result = {};
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        result[i].active = true;
        result[i].width = tracks[i]->video_width;
        result[i].height = tracks[i]->video_height;
        result[i].aspect = tracks[i]->video_aspect;
    }
    return result;
}

std::vector<LayoutTrackGeometryUpdate> update_layout_track_geometry_from_decision(
    TrackPipelineManager& tracks,
    const PresentDecision& decision) {
    std::vector<LayoutTrackGeometryUpdate> updates;

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i] || !decision.frames[i].has_value()) {
            continue;
        }
        const TextureFrame& frame = decision.frames[i].value();
        if (frame.width <= 0 || frame.height <= 0) {
            continue;
        }

        auto& track = *tracks[i];
        if (track.video_width == frame.width &&
            track.video_height == frame.height) {
            continue;
        }

        float sar = 1.0f;
        if (track.video_width > 0 &&
            track.video_height > 0 &&
            track.video_aspect > 0.0f) {
            const float old_natural_aspect =
                static_cast<float>(track.video_width) /
                static_cast<float>(track.video_height);
            if (old_natural_aspect > 0.0f) {
                sar = track.video_aspect / old_natural_aspect;
            }
        }
        if (!std::isfinite(sar) || sar <= 0.0f) {
            sar = 1.0f;
        }

        LayoutTrackGeometryUpdate update;
        update.slot = i;
        update.old_width = track.video_width;
        update.old_height = track.video_height;
        update.old_aspect = track.video_aspect;

        track.video_width = frame.width;
        track.video_height = frame.height;
        track.video_aspect =
            (static_cast<float>(frame.width) / static_cast<float>(frame.height)) * sar;
        if (!std::isfinite(track.video_aspect) || track.video_aspect <= 0.0f) {
            track.video_aspect =
                static_cast<float>(frame.width) / static_cast<float>(frame.height);
        }

        update.new_width = track.video_width;
        update.new_height = track.video_height;
        update.new_aspect = track.video_aspect;
        updates.push_back(update);
    }

    return updates;
}

std::pair<float, float> display_pixel_size_for_layout(
    int width,
    int height,
    const LayoutState& layout,
    const LayoutTrackGeometryList& tracks) {
    if (width <= 0 || height <= 0) {
        return {0.0f, 0.0f};
    }

    const int active_count = active_track_count(tracks);
    if (active_count == 0) {
        return {static_cast<float>(width), static_cast<float>(height)};
    }

    const int track_idx = first_display_track(layout, tracks);
    if (track_idx < 0 || !tracks[track_idx].active) {
        return {static_cast<float>(width), static_cast<float>(height)};
    }

    const float slot_w = slot_width_for_layout(width, layout, active_count);
    const float slot_h = static_cast<float>(height);
    const float slot_aspect = (slot_h > 0.0f) ? slot_w / slot_h : 1.0f;
    const float track_w = static_cast<float>(tracks[track_idx].width);
    const float track_h = static_cast<float>(tracks[track_idx].height);

    float track_scale = 1.0f;
    if (layout.pixel_size_mode == PIXEL_SIZE_UNIFORM_VIDEO_PIXELS) {
        track_scale = uniform_pixel_scale_for_track(tracks, track_idx, slot_w, slot_h);
    }

    float video_aspect = tracks[track_idx].aspect;
    if (video_aspect <= 0.0f) {
        video_aspect = (track_h > 0.0f) ? track_w / track_h : slot_aspect;
    }
    if (video_aspect <= 0.0f) {
        video_aspect = slot_aspect;
    }

    float fit_scale = (video_aspect > slot_aspect)
        ? slot_aspect / video_aspect
        : 1.0f;
    fit_scale *= track_scale;
    const float display_scale = fit_scale * layout.zoom_ratio;
    const float ds_x = (slot_aspect > 0.0f)
        ? video_aspect * display_scale / slot_aspect
        : display_scale;
    const float ds_y = display_scale;

    return {ds_x * slot_w, ds_y * slot_h};
}

LayoutResizeViewOffsetAdjustment adjust_layout_view_offset_for_resize(
    LayoutState& layout,
    int old_width,
    int old_height,
    int new_width,
    int new_height,
    const LayoutTrackGeometryList& tracks) {
    LayoutResizeViewOffsetAdjustment adjustment;
    adjustment.old_offset_x = layout.view_offset[0];
    adjustment.old_offset_y = layout.view_offset[1];

    const auto old_display = display_pixel_size_for_layout(
        old_width, old_height, layout, tracks);
    const auto new_display = display_pixel_size_for_layout(
        new_width, new_height, layout, tracks);
    if (old_display.first > 1e-4f && new_display.first > 1e-4f) {
        layout.view_offset[0] *= new_display.first / old_display.first;
        adjustment.adjusted_x = true;
    }
    if (old_display.second > 1e-4f && new_display.second > 1e-4f) {
        layout.view_offset[1] *= new_display.second / old_display.second;
        adjustment.adjusted_y = true;
    }

    adjustment.new_offset_x = layout.view_offset[0];
    adjustment.new_offset_y = layout.view_offset[1];
    return adjustment;
}

void populate_layout_shader_constants(ShaderConstants& constants,
                                      const LayoutState& layout,
                                      const LayoutTrackGeometryList& tracks,
                                      int canvas_width,
                                      int canvas_height) {
    constants.mode = layout.mode;
    constants.split_pos = layout.split_pos;
    constants.zoom_ratio = layout.zoom_ratio;
    constants.canvas_width = static_cast<float>(canvas_width);
    constants.canvas_height = static_cast<float>(canvas_height);
    constants.view_offset[0] = layout.view_offset[0];
    constants.view_offset[1] = layout.view_offset[1];

    for (int i = 0; i < 4; ++i) {
        constants.order[i] = layout.order[i];
    }

    const int active_count = active_track_count(tracks);
    constants.track_count = active_count;

    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        constants.video_aspect[i] = tracks[i].active ? tracks[i].aspect : 1.0f;
        constants.track_scale[i] = 1.0f;
    }

    const float slot_w = slot_width_for_layout(canvas_width, layout, active_count);
    const float slot_h = static_cast<float>(canvas_height);
    if (layout.pixel_size_mode == PIXEL_SIZE_UNIFORM_VIDEO_PIXELS) {
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
            if (!tracks[i].active) {
                continue;
            }
            constants.track_scale[i] = uniform_pixel_scale_for_track(tracks, i, slot_w, slot_h);
        }
    }

    const float slot_aspect = (slot_h > 0.0f) ? slot_w / slot_h : 1.0f;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        float video_aspect = constants.video_aspect[i];
        if (!std::isfinite(video_aspect) || video_aspect <= 0.0f) {
            video_aspect = slot_aspect;
        }

        float fit_scale = (video_aspect > slot_aspect)
            ? slot_aspect / video_aspect
            : 1.0f;
        fit_scale *= constants.track_scale[i];
        if (!std::isfinite(fit_scale) || fit_scale <= 0.0f) {
            fit_scale = 1.0f;
        }

        float display_scale = fit_scale * layout.zoom_ratio;
        if (!std::isfinite(display_scale) || display_scale <= 0.0f) {
            display_scale = 1.0f;
        }

        const float ds_x = (slot_aspect > 0.0f)
            ? video_aspect * display_scale / slot_aspect
            : display_scale;
        const float ds_y = display_scale;

        constants.display_offset_x[i] = (1.0f - ds_x) * 0.5f;
        constants.display_offset_y[i] = (1.0f - ds_y) * 0.5f;
        constants.inv_display_size_x[i] = (std::fabs(ds_x) > 1e-4f) ? 1.0f / ds_x : 0.0f;
        constants.inv_display_size_y[i] = (std::fabs(ds_y) > 1e-4f) ? 1.0f / ds_y : 0.0f;

        const float dp_x = ds_x * slot_w;
        const float dp_y = ds_y * slot_h;
        constants.view_offset_uv_x[i] =
            (std::fabs(dp_x) > 1e-4f) ? layout.view_offset[0] / dp_x : 0.0f;
        constants.view_offset_uv_y[i] =
            (std::fabs(dp_y) > 1e-4f) ? layout.view_offset[1] / dp_y : 0.0f;
    }
}

} // namespace vr
