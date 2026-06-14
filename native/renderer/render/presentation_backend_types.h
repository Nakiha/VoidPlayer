#pragma once

#include "renderer/color/color_strategy.h"
#include "renderer/decode/frame_identity_types.h"
#include "renderer/render/backend_type.h"

#include <cstdint>
#include <string>

namespace vr {

using PresentationBackendKind = RenderBackendKind;

struct PresentationBackendConfig {
    void* hwnd = nullptr;
    void* adapter = nullptr;
    void* output = nullptr;
    int width = 0;
    int height = 0;
    int max_track_slots = 0;
    bool headless = false;
    ColorOutputTarget output_target = ColorOutputTarget::kSDRToneMappedBT709;
    double sdr_white_level_nits = 80.0;
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
    int32_t color_range = 0;
    int32_t color_matrix = 0;
    int32_t color_transfer = 0;
    int32_t color_primaries = 0;
    uint64_t target_pixel_buffer_address = 0;
    uint64_t layout_revision = 0;
};

struct PresentationSourceFrameTarget {
    void* output = nullptr;
    int32_t source_slot = -1;
    int32_t source_file_id = -1;
    int32_t width = 0;
    int32_t height = 0;
    int32_t drawn = 0;
    PresentationBackendFrameInfo frame_info;
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

struct PresentationBackendDiagnostics {
    std::string backend;
    std::string target_format;
    std::string adapter_description;
    std::string driver_type;
    int32_t width = 0;
    int32_t height = 0;
    int32_t buffer_count = 0;
    int32_t adapter_vendor_id = 0;
    int32_t adapter_device_id = 0;
    int32_t adapter_luid_high = 0;
    uint32_t adapter_luid_low = 0;
    int32_t feature_level = 0;
    bool headless = false;
    bool warp = false;
};

inline bool is_transient_presentation_backpressure_error(const std::string& error) {
    return error == "renderer-owned Metal async draw deferred by backpressure" ||
           error == "native Metal uploader shared resources are busy" ||
           error == "native Metal uploader frame resource pool is busy" ||
           error == "native Metal uploader overlay layer resources are busy" ||
           error == "renderer-owned Metal presentation target ring is busy";
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

} // namespace vr
