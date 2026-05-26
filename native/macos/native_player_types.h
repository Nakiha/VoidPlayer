#ifndef VOIDPLAYER_MACOS_NATIVE_PLAYER_TYPES_H_
#define VOIDPLAYER_MACOS_NATIVE_PLAYER_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPMacOSNativePlayer VPMacOSNativePlayer;
typedef void (*VPMacOSFrameAvailableCallback)(void* user_data);

typedef struct VPMacOSNativeFrameInfo {
  int32_t width;
  int32_t height;
  int64_t pts_us;
  int64_t dts_us;
  int64_t duration_us;
} VPMacOSNativeFrameInfo;

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
  uint64_t target_generation;
  int32_t target_width;
  int32_t target_height;
  int32_t upload_storage_kind;
  int64_t last_successful_frame_pts_us;
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
  char codec_name[64];
  char decoder_name[128];
  char decode_mode[64];
} VPMacOSNativeTrackDiagnosticInfo;

typedef struct VPMacOSNativePlayerPerfStats {
  uint64_t process_rss_bytes;
  uint64_t process_private_bytes;
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
} VPMacOSNativePlayerPerfStats;

enum {
  VPMacOSNativeMaxTracks = 4,
};

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VOIDPLAYER_MACOS_NATIVE_PLAYER_TYPES_H_
