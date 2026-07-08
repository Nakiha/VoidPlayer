#pragma once

#include "windows/player/native_player.h"

#include <dxgi1_4.h>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vr {
struct WindowsSourceProjection;
} // namespace vr

class WindowsNativeCompositor {
public:
    enum class OutputTarget {
        SDR,
        ScRGB,
    };

    enum class Phase {
        Inactive,
        Preparing,
        Active,
        Failed,
    };

    struct Diagnostics {
        std::string phase = "inactive";
        std::string fallback_reason = "native-compositor-removed";
        std::string source_cache_last_error = "native-compositor-removed";
        std::string output_target = "sdr";
        std::string desired_output_target = "sdr";
        std::string transition_state = "stable";
        std::string transition_reason = "flutter-texture-sdr";
        std::string swap_chain_format = "none";
        std::string color_space = "flutter-texture-sdr";
        std::string producer_adapter_luid = "0:0";
        std::string output_adapter_luid = "0:0";
        std::string pending_output_adapter_luid = "0:0";
        std::string cross_adapter_transport_mode = "disabled";
        std::string cross_adapter_transport_status = "not-required";
        std::string cross_adapter_sync_kind = "none";
        std::string cross_adapter_requested_sync_kind = "none";
        std::string cross_adapter_active_sync_kind = "none";
        std::string cross_adapter_sync_fallback_reason = "native-compositor-removed";
        std::string cross_adapter_last_error = "none";
        std::string device_recovery_state = "disabled";
        std::string device_recovery_last_reason = "none";
        std::string device_recovery_last_removed_reason = "0x00000000";
        std::string device_recovery_fallback_stage = "none";
        uint64_t state_serial = 0;
        uint64_t ack_serial = 0;
        uint64_t transition_serial = 0;
        uint64_t device_recovery_generation = 0;
        uint64_t device_recovery_attempt_count = 0;
        uint64_t device_recovery_success_count = 0;
        uint64_t device_recovery_failure_count = 0;
        uint64_t device_recovery_last_duration_ms = 0;
        uint64_t output_generation = 0;
        uint64_t output_migration_count = 0;
        uint64_t output_migration_failure_count = 0;
        uint64_t hdr_promotion_count = 0;
        uint64_t hdr_demotion_count = 0;
        uint64_t target_fallback_count = 0;
        uint64_t transport_generation = 0;
        uint64_t transport_copy_count = 0;
        uint64_t transport_copy_bytes = 0;
        uint64_t transport_fence_wait_count = 0;
        uint64_t transport_timeout_count = 0;
        uint64_t transport_last_copy_us = 0;
        uint64_t transport_total_copy_us = 0;
        uint64_t shared_fence_signal_count = 0;
        uint64_t shared_fence_wait_count = 0;
        uint64_t shared_fence_timeout_count = 0;
        uint64_t shared_fence_last_wait_us = 0;
        uint64_t shared_fence_p95_wait_us = 0;
        uint64_t event_query_p95_wait_us = 0;
        uint64_t flutter_transport_generation = 0;
        uint64_t video_transport_generation = 0;
        uint64_t source_transport_generation = 0;
        uint64_t flutter_generation = 0;
        uint64_t flutter_export_state_generation = 0;
        uint64_t flutter_export_ring_generation = 0;
        uint64_t flutter_export_publish_count = 0;
        uint64_t flutter_export_request_count = 0;
        uint64_t flutter_export_request_dispatch_count = 0;
        uint64_t flutter_export_schedule_frame_count = 0;
        uint64_t flutter_export_vsync_count = 0;
        uint64_t flutter_export_present_count = 0;
        uint64_t flutter_export_begin_count = 0;
        uint64_t flutter_export_begin_fail_count = 0;
        uint64_t flutter_export_make_current_fail_count = 0;
        uint64_t flutter_export_publish_fail_count = 0;
        uint64_t flutter_export_flush_count = 0;
        uint64_t flutter_export_finish_count = 0;
        uint64_t flutter_export_backpressure_count = 0;
        uint64_t flutter_export_pending_frame_pump_frames = 0;
        uint64_t flutter_export_stale_timeout_count = 0;
        uint64_t flutter_export_unsolicited_signal_count = 0;
        uint64_t flutter_export_unsolicited_throttle_count = 0;
        uint64_t video_generation = 0;
        uint64_t composite_count = 0;
        uint64_t present_count = 0;
        uint64_t drop_count = 0;
        uint64_t failure_count = 0;
        uint64_t resize_count = 0;
        uint64_t source_cache_consumed_generation = 0;
        uint64_t source_cache_fallback_count = 0;
        uint64_t source_projection_update_count = 0;
        uint64_t overlay_generation = 0;
        uint64_t overlay_fill_rect_count = 0;
        uint64_t overlay_line_rect_count = 0;
        uint64_t overlay_motion_line_count = 0;
        int64_t high_refresh_display_hz = 60;
        int64_t dcomp_present_interval_p95_us = 0;
        int64_t dcomp_composite_p95_us = 0;
        int64_t dcomp_draw_p95_us = 0;
        int64_t dcomp_present_block_p95_us = 0;
        int64_t dcomp_acquire_wait_p95_us = 0;
        int64_t interaction_input_to_present_p95_us = 0;
        int64_t dcomp_drop_rate_x1000 = 0;
        uint64_t source_projection_reuse_count = 0;
        uint64_t viewport_redraw_during_projection_count = 0;
        uint64_t overlay_layer_raster_count = 0;
        uint64_t overlay_layer_upload_count = 0;
        uint64_t overlay_layer_reuse_count = 0;
        uint64_t overlay_layer_texture_count = 0;
        uint64_t overlay_layer_bytes = 0;
        uint64_t overlay_layer_generation = 0;
        uint64_t overlay_layer_committed_generation = 0;
        uint64_t overlay_layer_pending_generation = 0;
        uint64_t overlay_layer_composite_count = 0;
        uint64_t overlay_layer_miss_count = 0;
        uint64_t overlay_layer_backpressure_count = 0;
        int64_t overlay_composite_p95_us = 0;
        int64_t overlay_raster_p95_us = 0;
        int64_t overlay_upload_p95_us = 0;
        int64_t hot_path_display_hz = 60;
        int64_t hot_path_frame_budget_us = 16666;
        int64_t hot_path_present_interval_p95_us = 0;
        int64_t hot_path_composite_p95_us = 0;
        int64_t hot_path_draw_p95_us = 0;
        int64_t hot_path_present_block_p95_us = 0;
        int64_t hot_path_acquire_wait_p95_us = 0;
        int64_t hot_path_input_to_present_p95_us = 0;
        int64_t hot_path_drop_rate_x1000 = 0;
        uint64_t hot_path_projection_only_update_count = 0;
        uint64_t hot_path_viewport_redraw_during_projection_count = 0;
        uint64_t hot_path_source_cache_reuse_count = 0;
        uint64_t hot_path_overlay_reuse_count = 0;
        uint64_t hot_path_overlay_raster_count = 0;
        uint64_t hot_path_overlay_upload_count = 0;
        double source_cache_hz = 0.0;
        double source_projection_hz = 0.0;
        std::string overlay_layer_mode = "inactive";
        std::string overlay_layer_fallback_reason = "native-compositor-removed";
        std::string overlay_layer_last_error = "none";
        std::string high_refresh_gate_last_result = "not-run";
        std::string hot_path_mode = "inactive";
        std::string hot_path_last_failure_reason = "native-compositor-removed";
        std::string hot_path_gate_result = "not-run";
        std::string retained_graph_mode = "inactive";
        std::string retained_graph_fallback_reason = "native-compositor-removed";
        bool engine_export_available = false;
        bool engine_export_frame_pump_available = false;
        bool flutter_export_latest_available = false;
        bool swap_chain_active = false;
        bool color_space_supported = false;
        bool sdr_tone_map_active = false;
        bool cross_adapter_required = false;
        bool cross_adapter_supported = false;
        bool device_recovery_preserved_player = true;
        bool device_recovery_last_frame_held = false;
        bool transport_bgra8_supported = false;
        bool transport_fp16_supported = false;
        bool transport_shared_fence_supported = false;
        bool transport_shared_fence_producer_supported = false;
        bool transport_shared_fence_output_supported = false;
        bool transport_shared_fence_handle_created = false;
        bool transport_shared_fence_open_succeeded = false;
        bool source_projection_enabled = false;
        bool source_cache_active = false;
        bool high_refresh_gate_supported = false;
        bool overlay_retained_layer_active = false;
        bool hot_path_active = false;
        bool retained_graph_active = false;
        uint64_t retained_graph_commit_count = 0;
        uint64_t retained_graph_projection_commit_count = 0;
        uint64_t retained_graph_source_bake_count = 0;
        uint64_t retained_graph_flutter_bake_count = 0;
        uint64_t retained_graph_projection_skip_present_count = 0;
        uint64_t retained_graph_deferred_content_count = 0;
        uint64_t retained_graph_commit_defer_count = 0;
        int64_t retained_graph_flutter_bake_p95_us = 0;
        int64_t retained_graph_source_bake_p95_us = 0;
        int64_t retained_graph_apply_p95_us = 0;
        int64_t retained_graph_commit_p95_us = 0;
        uint32_t swap_chain_width = 0;
        uint32_t swap_chain_height = 0;
    };

    using StateCallback = std::function<void(Phase, uint64_t, const std::string&)>;
    using SourceProjection = vr::WindowsSourceProjection;

    WindowsNativeCompositor() = default;
    ~WindowsNativeCompositor() = default;

    bool Start(HWND,
               void*,
               const std::shared_ptr<vr::NativePlayer>&,
               IDXGIAdapter*,
               IDXGIAdapter*,
               double,
               OutputTarget,
               StateCallback callback) {
        if (callback) {
            callback(Phase::Inactive, 0, "native-compositor-removed");
        }
        return false;
    }
    void Stop(const char* = "shutdown") {}
    void SetViewportRect(double, double, double, double) {}
    void SetViewportBackgroundColor(uint32_t) {}
    void NotifyClientSizeChanged(uint32_t, uint32_t) {}
    bool RequestFlutterFrame(const std::string&) { return false; }
    void BoostFlutterInteraction(const std::string&) {}
    void DisableRetainedSourceProjection(const std::string&) {}
    void ClearSourceProjection(const std::string&) {}
    void SetSourceCacheError(const std::string&) {}
    void RequestOutputTarget(OutputTarget, IDXGIAdapter*, double, uint64_t, const std::string&) {}
    void AcknowledgeFlutterState(uint64_t, bool) {}
    void ForceFailureForTesting(const std::string&) {}
    bool BeginDeviceRecovery(const std::string&, long) { return false; }
    void SetHighRefreshDisplayHz(int64_t) {}
    void ResetHighRefreshMetrics() {}
    void BeginInteractionSample(const std::string&) {}
    void EndInteractionSample(const std::string&) {}
    Diagnostics diagnostics() const { return {}; }
};
