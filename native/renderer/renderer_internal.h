#pragma once
#include "renderer/renderer.h"
#include "renderer/overlay/analysis_overlay_renderer.h"
#include "renderer/layout/layout_geometry.h"
#include "renderer/layout/layout_validation.h"
#include "renderer/renderer_config_validation.h"
#include "renderer/playback/renderer_playback_command_policy.h"
#include "renderer/seek/renderer_seek_log_policy.h"
#include "renderer/track/track_lifecycle.h"
#include "renderer/track/track_preroll_policy.h"
#include "renderer/track/track_present_policy.h"
#include "renderer/track/track_preview_policy.h"
#include "renderer/track/track_step_policy.h"
#include "audio/audio_output_factory.h"
#include "renderer/audio_coordinator.h"
#include "renderer/seek/seek_coordinator.h"
#include "renderer/render/device_loss_policy.h"
#include "renderer/render/presentation_backend_factory.h"
#include "renderer/render/presentation_snapshot.h"
#include "renderer/render/render_thread_platform.h"
#include "renderer/render/swap_chain_present_policy.h"
#include "renderer/track/track_snapshot.h"
#ifdef _WIN32
#include "renderer/capture/frame_capture_service.h"
#include "windows/d3d11/render_backend.h"
#endif
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <utility>

namespace vr {

// Max render-loop sleep for playback deadline responsiveness. Viewport layout
// redraw cadence is driven by the platform display clock on macOS.
static constexpr int64_t MAX_SLEEP_US = 8000;
static constexpr auto kPausedHevcSeekSettleDelay = std::chrono::milliseconds(250);
static constexpr auto kStepForwardDecodeWait = std::chrono::milliseconds(180);
static constexpr auto kTransientPresentationBackpressureBackoff =
    std::chrono::microseconds(MAX_SLEEP_US);
static constexpr auto kViewportCompositorActivityGrace =
    std::chrono::milliseconds(25);

inline uint64_t elapsed_us_since(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}

inline uint64_t percentile_95_us(std::vector<uint64_t> samples) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const auto index = std::min(
        samples.size() - 1,
        static_cast<size_t>(std::ceil(static_cast<double>(samples.size()) * 0.95) - 1.0));
    return samples[index];
}

inline void atomic_fetch_max(std::atomic<uint64_t>& target, uint64_t value) {
    auto current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(
               current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

inline int64_t steady_clock_us_now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline bool profiler_enabled(const char* env_name) {
    const char* value = std::getenv(env_name);
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0;
}

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
    case FrameStorageKind::D3D11Nv12:
        return "d3d11-nv12";
    case FrameStorageKind::D3D11Texture:
        return "d3d11-texture";
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

inline void stop_detached_track_pipeline(size_t slot, std::unique_ptr<TrackPipeline>& track) {
    if (!track) {
        return;
    }
    if (track->decode_thread) {
        spdlog::info("Renderer: stopping track[{}] decode ({})", slot, track->file_path);
        track->decode_thread->stop();
        spdlog::info("Renderer: track[{}] decode stopped", slot);
    }
    if (track->demux_thread) {
        spdlog::info("Renderer: stopping track[{}] demux ({})", slot, track->file_path);
        track->demux_thread->stop();
        spdlog::info("Renderer: track[{}] demux stopped", slot);
    }
    track.reset();
}

} // namespace vr
