#ifndef VOIDPLAYER_MACOS_NATIVE_PLAYER_TYPES_H_
#define VOIDPLAYER_MACOS_NATIVE_PLAYER_TYPES_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPMacOSNativePlayer VPMacOSNativePlayer;
typedef void (*VPMacOSFrameAvailableCallback)(void* user_data);

#define VP_MACOS_NATIVE_API_VERSION 9u

typedef enum VPMacOSNativeStatus {
  VPMacOSNativeStatusOk = 0,
  VPMacOSNativeStatusInvalidArgument = -1,
  VPMacOSNativeStatusUnsupported = -2,
  VPMacOSNativeStatusTransientBackpressure = -3,
  VPMacOSNativeStatusRendererFailed = -4,
} VPMacOSNativeStatus;

typedef struct VPMacOSNativeFrameInfo {
  uint32_t struct_size;
  uint32_t api_version;
  int32_t width;
  int32_t height;
  int64_t pts_us;
  int64_t dts_us;
  int64_t duration_us;
  int32_t analysis_frame_index;
  int32_t frame_identity_mode;
  int32_t source_packet_index;
  int32_t source_packet_size;
  int64_t source_packet_pos;
  int64_t source_packet_pts;
  int64_t source_packet_dts;
  int32_t color_range;
  int32_t color_matrix;
  int32_t color_transfer;
  int32_t color_primaries;
  uint64_t target_pixel_buffer_address;
  uint64_t layout_revision;
} VPMacOSNativeFrameInfo;

static inline void VPMacOSNativeFrameInfoInit(VPMacOSNativeFrameInfo* out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  out->struct_size = (uint32_t)sizeof(VPMacOSNativeFrameInfo);
  out->api_version = VP_MACOS_NATIVE_API_VERSION;
}

typedef struct VPMacOSNativeSourceFrameBakeTarget {
  void* pixel_buffer;
  int32_t source_slot;
  int32_t source_file_id;
  int32_t width;
  int32_t height;
  int32_t drawn;
  VPMacOSNativeFrameInfo frame_info;
} VPMacOSNativeSourceFrameBakeTarget;

typedef struct VPMacOSNativeOverlayPrimitiveSnapshot {
  uint32_t struct_size;
  uint32_t api_version;
  uint64_t generation;
  size_t fill_rect_count;
  size_t line_rect_count;
  size_t motion_line_count;
  int32_t source_baked_overlay_disabled;
  uint64_t overlay_track_count;
  uint64_t matched_track_count;
  uint64_t missing_track_slot_count;
  uint64_t missing_presented_frame_count;
  uint64_t missing_frame_index_count;
  uint64_t invalid_video_size_count;
  uint64_t overlay_frame_missing_count;
  uint64_t heatmap_missing_feature_track_count;
} VPMacOSNativeOverlayPrimitiveSnapshot;

static inline void VPMacOSNativeOverlayPrimitiveSnapshotInit(
    VPMacOSNativeOverlayPrimitiveSnapshot* out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  out->struct_size = (uint32_t)sizeof(VPMacOSNativeOverlayPrimitiveSnapshot);
  out->api_version = VP_MACOS_NATIVE_API_VERSION;
  out->source_baked_overlay_disabled = 1;
}

typedef struct VPMacOSNativeTrackInfo {
  int32_t file_id;
  int32_t slot;
  int32_t width;
  int32_t height;
  int64_t duration_us;
  int64_t start_time_us;
  int64_t bit_rate;
  char format_name[64];
  char codec_name[64];
  char codec_long_name[128];
  char decoder_name[128];
  int32_t color_range;
  int32_t color_matrix;
  int32_t color_transfer;
  int32_t color_primaries;
} VPMacOSNativeTrackInfo;

typedef struct VPMacOSCaptureMetrics {
  int32_t width;
  int32_t height;
  double avg_luma;
  double non_black_ratio;
  uint64_t hash;
} VPMacOSCaptureMetrics;

typedef struct VPMacOSNativeLayoutState {
  int32_t mode;
  float split_pos;
  float zoom_ratio;
  float view_offset_x;
  float view_offset_y;
  int32_t pixel_size_mode;
  int32_t order[4];
} VPMacOSNativeLayoutState;

typedef struct VPMacOSNativeLayoutPresentationParams {
  float display_offset_x;
  float display_offset_y;
  float inv_display_size_x;
  float inv_display_size_y;
  float view_offset_uv_x;
  float view_offset_uv_y;
} VPMacOSNativeLayoutPresentationParams;

typedef struct VPMacOSNativePresentationSchedulerStats {
  uint64_t tick_count;
  uint64_t presentable_tick_count;
  uint64_t frame_notification_count;
  int64_t last_selected_pts_us;
  int32_t last_present_frame_count;
  int32_t cached_present_decision_available;
  uint64_t deadline_sleep_count;
  int64_t last_deadline_sleep_us;
} VPMacOSNativePresentationSchedulerStats;

typedef struct VPMacOSNativeRendererOwnedPresentationState {
  int32_t renderer_initialized;
  int32_t target_installed;
  int32_t backend_available;
  int32_t last_draw_succeeded;
  uint64_t consecutive_draw_failures;
  uint64_t draw_failure_count;
  uint64_t upload_count;
  uint64_t upload_failure_count;
  uint64_t upload_interval_p95_ms;
  uint64_t target_generation;
  uint64_t target_warmup_generation;
  uint64_t target_warmup_remaining;
  uint64_t target_warmup_sample_count;
  uint64_t target_warmup_last_ms;
  uint64_t target_warmup_p95_ms;
  int32_t target_width;
  int32_t target_height;
  int32_t upload_storage_kind;
  int64_t last_successful_frame_pts_us;
  int32_t overlay_last_expected;
  int32_t overlay_last_applied;
  uint64_t overlay_last_fill_rect_count;
  uint64_t overlay_last_line_rect_count;
  uint64_t overlay_expected_count;
  uint64_t overlay_applied_count;
  uint64_t overlay_missed_count;
  uint64_t overlay_gpu_success_count;
  uint64_t overlay_gpu_failure_count;
  uint64_t overlay_cpu_fallback_count;
  uint64_t external_flutter_surface_generation;
  uint64_t external_flutter_surface_consumed_generation;
  uint64_t external_flutter_surface_update_count;
  uint64_t external_flutter_surface_consume_count;
  uint64_t external_flutter_surface_wait_count;
  uint64_t external_flutter_surface_wait_failure_count;
  int32_t source_cache_active;
  int32_t source_cache_texture_count;
  uint64_t source_cache_generation;
  uint64_t source_cache_publish_count;
  int32_t source_projection_active;
  uint64_t source_projection_update_count;
  uint64_t source_projection_consume_count;
  char backend_name[64];
  char external_flutter_surface_last_error[128];
  char last_draw_error[256];
} VPMacOSNativeRendererOwnedPresentationState;

typedef struct VPMacOSNativeTrackDiagnosticInfo {
  int32_t file_id;
  int32_t slot;
  int32_t width;
  int32_t height;
  int64_t duration_us;
  int64_t offset_us;
  int32_t hardware_decode_active;
  int32_t hardware_decode_downloads_to_cpu;
  int32_t buffer_state;
  uint64_t buffer_count;
  uint64_t buffer_capacity;
  uint64_t cpu_frame_memory_bytes;
  uint64_t packet_queue_memory_bytes;
  uint64_t frames_decoded;
  double decode_fps;
  double decode_avg_ms;
  double decode_max_ms;
  int64_t current_pts_us;
  int64_t current_dts_us;
  int32_t analysis_frame_index;
  int32_t frame_identity_mode;
  int32_t source_packet_index;
  int32_t source_packet_size;
  int64_t source_packet_pos;
  int64_t source_packet_pts;
  int64_t source_packet_dts;
  int32_t color_range;
  int32_t color_matrix;
  int32_t color_transfer;
  int32_t color_primaries;
  char codec_name[64];
  char decoder_name[128];
  char decode_mode[64];
} VPMacOSNativeTrackDiagnosticInfo;

typedef struct VPMacOSNativePlayerPerfStats {
  uint64_t process_rss_bytes;
  uint64_t process_private_bytes;
  uint64_t dedicated_gpu_usage_bytes;
  uint64_t decode_frame_count;
  uint64_t decode_dropped_count;
  int64_t decode_elapsed_ms;
  double decode_fps;
  double decode_avg_ms;
  double decode_max_ms;
  uint64_t renderer_owned_upload_count;
  uint64_t renderer_owned_upload_failure_count;
  int64_t renderer_owned_upload_elapsed_ms;
  double renderer_owned_upload_fps;
  int64_t renderer_owned_direct_yuv_upload_count;
  int64_t renderer_owned_cvpixelbuffer_upload_count;
  int64_t renderer_owned_present_package_upload_count;
  int64_t renderer_owned_present_package_copy_us;
  int64_t renderer_owned_present_package_gpu_wait_us;
  int64_t renderer_owned_present_package_total_us;
  int32_t renderer_owned_present_package_storage;
  uint64_t active_track_count;
  uint64_t aggregate_decode_frame_count;
  double aggregate_decode_fps;
  uint64_t cpu_frame_memory_bytes;
  uint64_t packet_queue_memory_bytes;
  uint64_t renderer_owned_staging_allocation_count;
  uint64_t renderer_owned_staging_reuse_count;
  uint64_t renderer_owned_staging_max_bytes;
  uint64_t renderer_draw_count;
  int64_t renderer_draw_avg_us;
  int64_t renderer_draw_max_us;
  int64_t renderer_draw_p95_us;
  int64_t renderer_draw_backend_avg_us;
  int64_t renderer_draw_backend_max_us;
  int64_t renderer_draw_backend_p95_us;
  uint64_t renderer_layout_intent_count;
  uint64_t renderer_layout_presented_count;
  uint64_t renderer_layout_deferred_to_playback_count;
  uint64_t renderer_playing_layout_redraw_suppressed_count;
  uint64_t renderer_layout_stale_completion_drop_count;
  uint64_t renderer_last_layout_revision;
  uint64_t renderer_last_presented_layout_revision;
  int64_t renderer_draws_per_presented_layout_x1000;
  uint64_t in_flight_metal_buffer_count;
  uint64_t metal_buffer_exhaustion_count;
  uint64_t metal_command_completion_p95_us;
  uint64_t metal_command_failure_count;
  uint64_t wgpu_compose_total_p95_us;
  uint64_t wgpu_compose_pre_render_p95_us;
  uint64_t wgpu_compose_import_p95_us;
  uint64_t wgpu_compose_prepare_p95_us;
  uint64_t wgpu_compose_overlay_encode_p95_us;
  uint64_t wgpu_compose_bind_group_p95_us;
  uint64_t wgpu_compose_pass_encode_p95_us;
  uint64_t wgpu_compose_submit_p95_us;
  uint64_t wgpu_compose_cpu_render_p95_us;
  int32_t async_metal_publish_active;
  uint64_t video_source_update_count;
  uint64_t viewport_composite_count;
  uint64_t source_frame_cache_hit_count;
  uint64_t source_frame_cache_miss_count;
  uint64_t source_frame_stale_completion_drop_count;
} VPMacOSNativePlayerPerfStats;

typedef struct VPMacOSNativeAudioDiagnostics {
  int32_t device_initialized;
  int32_t playing;
  int32_t active_track;
  int32_t output_sample_rate;
  int32_t output_channels;
  uint64_t registered_track_count;
  int32_t active_track_registered;
  uint64_t active_track_queued_frames;
  int64_t active_track_queued_duration_us;
  uint64_t active_track_underrun_frames;
  uint64_t active_track_discarded_frames;
  uint64_t active_track_seek_trimmed_frames;
} VPMacOSNativeAudioDiagnostics;

enum {
  VPMacOSNativeMaxTracks = 4,
};

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VOIDPLAYER_MACOS_NATIVE_PLAYER_TYPES_H_
