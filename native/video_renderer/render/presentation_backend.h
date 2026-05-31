#pragma once

#include "video_renderer/render/backend_type.h"
#include "video_renderer/render/renderer_draw_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vr {

using PresentationBackendKind = RenderBackendKind;
class PresentationBackend;

struct PresentationBackendConfig {
    void* hwnd = nullptr;
    void* adapter = nullptr;
    void* output = nullptr;
    int width = 0;
    int height = 0;
    int max_track_slots = 0;
    bool headless = false;
};

struct PresentationBackendFrameInfo {
    int32_t width = 0;
    int32_t height = 0;
    int64_t pts_us = 0;
    int64_t dts_us = 0;
    int64_t duration_us = 0;
    int32_t analysis_frame_index = kInvalidAnalysisFrameIndex;
    int32_t frame_identity_mode = static_cast<int32_t>(FrameIdentityMode::Unknown);
    int32_t source_packet_index = kInvalidSourcePacketIndex;
    int32_t source_packet_size = 0;
    int64_t source_packet_pos = kUnknownSourcePacketPosition;
    int64_t source_packet_pts = kNoTimestampUs;
    int64_t source_packet_dts = kNoTimestampUs;
    uint64_t target_pixel_buffer_address = 0;
};

using PresentationBackendAsyncDrawCompleted =
    std::function<void(bool, const char*, uint64_t, const PresentationBackendFrameInfo*)>;

struct PresentationBackendDrawHooks {
    const char* draw_source = nullptr;
    std::function<void(const char*)> wait_gpu_idle;
    std::function<void(uint64_t)> record_frame_copy_us;
    std::function<void(PresentationBackend&, const RendererDrawSnapshot&)> draw_overlay;
    std::function<bool(const RendererDrawSnapshot&, uint8_t*, int, int, size_t)> composite_bgra_overlay;
    PresentationBackendAsyncDrawCompleted async_draw_completed;
};

struct PresentationBackendStats {
    int64_t direct_yuv_upload_count = 0;
    int64_t cvpixelbuffer_upload_count = 0;
    int64_t present_package_upload_count = 0;
    int64_t last_present_package_copy_us = 0;
    int64_t last_present_package_gpu_wait_us = 0;
    int64_t last_present_package_total_us = 0;
    int32_t last_present_package_storage = 0;
    int32_t backend_available = 0;
    int32_t target_installed = 0;
    int32_t last_draw_succeeded = 0;
    uint64_t draw_failure_count = 0;
    uint64_t consecutive_draw_failures = 0;
    int64_t last_successful_frame_pts_us = 0;
    uint64_t staging_allocation_count = 0;
    uint64_t staging_reuse_count = 0;
    uint64_t staging_max_bytes = 0;
    int32_t overlay_last_expected = 0;
    int32_t overlay_last_applied = 0;
    uint64_t overlay_last_fill_rect_count = 0;
    uint64_t overlay_last_line_rect_count = 0;
    uint64_t overlay_expected_count = 0;
    uint64_t overlay_applied_count = 0;
    uint64_t overlay_missed_count = 0;
    uint64_t overlay_gpu_success_count = 0;
    uint64_t overlay_gpu_failure_count = 0;
    uint64_t overlay_cpu_fallback_count = 0;
    uint64_t in_flight_metal_buffer_count = 0;
    uint64_t metal_buffer_exhaustion_count = 0;
    uint64_t metal_command_completion_p95_us = 0;
    uint64_t metal_command_failure_count = 0;
    int32_t async_metal_publish_active = 0;
    uint64_t video_source_update_count = 0;
    uint64_t viewport_composite_count = 0;
    uint64_t source_frame_cache_hit_count = 0;
    uint64_t source_frame_cache_miss_count = 0;
};

inline bool is_transient_presentation_backpressure_error(const std::string& error) {
    return error == "renderer-owned Metal async draw deferred by backpressure";
}

struct PresentationBackendMetrics {
    uint64_t draw_count = 0;
    uint64_t draw_total_us = 0;
    uint64_t draw_max_us = 0;
    uint64_t draw_p95_us = 0;
    uint64_t draw_backend_total_us = 0;
    uint64_t draw_backend_max_us = 0;
    uint64_t draw_backend_p95_us = 0;
    uint64_t render_wait_us = 0;
    uint64_t render_wait_count = 0;
    uint64_t frame_copy_us = 0;
    uint64_t frame_copy_count = 0;
    uint64_t present_publish_us = 0;
    uint64_t present_publish_count = 0;
    uint64_t shared_texture_resize_count = 0;
    uint64_t device_lost_count = 0;
    uint64_t texture_sharing_failure_count = 0;
    uint64_t layout_intent_count = 0;
    uint64_t layout_presented_count = 0;
    uint64_t layout_deferred_to_playback_count = 0;
    uint64_t playing_layout_redraw_suppressed_count = 0;
    uint64_t layout_stale_completion_drop_count = 0;
    uint64_t last_layout_revision = 0;
    uint64_t last_presented_layout_revision = 0;
};

class PresentationBackend {
public:
    virtual ~PresentationBackend() = default;

    virtual PresentationBackendKind kind() const = 0;
    virtual const char* name() const = 0;
    virtual bool initialize(const PresentationBackendConfig& config) = 0;
    virtual void shutdown() = 0;
    virtual bool headless() const = 0;
    virtual bool renderer_manages_headless_publish() const { return false; }
    virtual bool completes_draw_asynchronously() const { return false; }
    virtual bool supports_swap_chain_present() const { return false; }
    virtual bool poll_device_removed(const char*) { return false; }
    virtual bool device_lost() const { return false; }
    virtual long device_removed_reason() const { return 0; }
    virtual void wait_idle(const char*) {}
    virtual bool present_swap_chain(int) { return false; }
    virtual void reset_track(size_t) {}
    virtual void move_track(size_t, size_t) {}
    virtual bool update_headless_output(void*, int, int, int) { return false; }
    virtual void clear_headless_output() {}
    virtual PresentationBackendStats presentation_stats() const { return {}; }
    virtual bool copy_last_frame_info(PresentationBackendFrameInfo*) const { return false; }
    virtual bool capture_front_buffer(std::vector<uint8_t>&, int&, int&) { return false; }
    virtual const char* last_error() const { return ""; }
    virtual bool draw_frame(const RendererDrawSnapshot& snapshot,
                            const PresentationBackendDrawHooks& hooks) = 0;
};

} // namespace vr
