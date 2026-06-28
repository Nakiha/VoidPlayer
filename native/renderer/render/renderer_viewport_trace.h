#pragma once

#include "renderer/render/renderer_draw_snapshot.h"
#include "renderer/render/renderer_profiler_flags.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <spdlog/spdlog.h>

namespace vr {

inline bool viewport_trace_enabled() {
    return profiler_enabled("VOIDPLAYER_VIEWPORT_TRACE");
}

inline bool viewport_trace_log_all() {
    return profiler_enabled("VOIDPLAYER_VIEWPORT_TRACE_ALL");
}

inline bool should_log_viewport_trace_event(bool important) {
    if (!viewport_trace_enabled()) {
        return false;
    }
    if (viewport_trace_log_all() || important) {
        return true;
    }
    static std::atomic<uint64_t> trace_event_count{0};
    const auto count = trace_event_count.fetch_add(1, std::memory_order_relaxed) + 1;
    return count % 120 == 0;
}

inline const char* frame_storage_kind_name(FrameStorageKind kind) {
    switch (kind) {
    case FrameStorageKind::CpuRgba:
        return "cpu-rgba";
    case FrameStorageKind::CpuNv12:
        return "cpu-nv12";
    case FrameStorageKind::CpuPlanarYuv:
        return "cpu-planar-yuv";
    case FrameStorageKind::D3D12Texture:
        return "d3d12-texture";
    case FrameStorageKind::MacOSCVPixelBuffer:
        return "macos-cvpixelbuffer";
    case FrameStorageKind::Empty:
    default:
        return "empty";
    }
}

inline int first_present_slot(const RendererDrawSnapshot& snapshot) {
    for (size_t i = 0; i < snapshot.decision.frames.size(); ++i) {
        if (snapshot.decision.frames[i].has_value()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline size_t present_decision_frame_count(const PresentDecision& decision) {
    size_t count = 0;
    for (const auto& frame : decision.frames) {
        if (frame.has_value()) {
            ++count;
        }
    }
    return count;
}

inline void log_viewport_draw_trace(const char* source,
                                    const RendererDrawSnapshot& snapshot,
                                    uint64_t snapshot_layout_revision,
                                    uint64_t current_layout_revision,
                                    bool attempted_draw,
                                    bool drew,
                                    bool stale_layout_after_draw,
                                    bool callback_available,
                                    bool callback_published,
                                    uint64_t total_us,
                                    uint64_t snapshot_us,
                                    uint64_t backend_us) {
    const bool slow_or_abnormal =
        stale_layout_after_draw || !callback_published || !drew || total_us >= 8000 ||
        backend_us >= 6000;
    if (!should_log_viewport_trace_event(slow_or_abnormal)) {
        return;
    }
    const int slot = first_present_slot(snapshot);
    int file_id = -1;
    int64_t pts_us = snapshot.decision.current_pts_us;
    const char* storage = "none";
    if (slot >= 0) {
        const auto index = static_cast<size_t>(slot);
        file_id = snapshot.decision.file_ids[index];
        if (snapshot.decision.frames[index].has_value()) {
            const auto& frame = snapshot.decision.frames[index].value();
            pts_us = frame.pts_us;
            storage = frame_storage_kind_name(frame.storage_kind());
        }
    }
    spdlog::info(
        "[ViewportTrace] native source={} attempted={} drew={} stale={} "
        "callback_available={} callback_published={} total_us={} snapshot_us={} "
        "backend_us={} layout_rev={} current_layout_rev={} slot={} file_id={} "
        "pts_us={} storage={} target={}x{} mode={} zoom={:.4f} "
        "offset=({:.1f},{:.1f}) split={:.4f} pixel_mode={}",
        source,
        attempted_draw,
        drew,
        stale_layout_after_draw,
        callback_available,
        callback_published,
        total_us,
        snapshot_us,
        backend_us,
        snapshot_layout_revision,
        current_layout_revision,
        slot,
        file_id,
        pts_us,
        storage,
        snapshot.target_width,
        snapshot.target_height,
        snapshot.layout.mode,
        snapshot.layout.zoom_ratio,
        snapshot.layout.view_offset[0],
        snapshot.layout.view_offset[1],
        snapshot.layout.split_pos,
        snapshot.layout.pixel_size_mode);
}

} // namespace vr
