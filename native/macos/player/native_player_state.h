#ifndef VOIDPLAYER_MACOS_NATIVE_PLAYER_STATE_H_
#define VOIDPLAYER_MACOS_NATIVE_PLAYER_STATE_H_

#include "macos/metal/metal_presentation_backend_bridge.h"
#include "macos/player/native_player_types.h"
#include "renderer/renderer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vp_macos {

void write_error(char* error, size_t error_size, const std::string& message);
bool videotoolbox_disabled_by_env();
bool videotoolbox_hwdownload_forced_by_env();
bool probe_videotoolbox_h264();
bool decoder_name_is_videotoolbox(const std::string& decoder_name);
vr::LayoutState to_layout_state(const VPMacOSNativeLayoutState& state);
VPMacOSNativeLayoutState to_native_layout_state(const vr::LayoutState& layout);

}  // namespace vp_macos

struct VPMacOSNativePlayer {
  VPMacOSNativePlayer() = default;
  ~VPMacOSNativePlayer();

  VPMacOSNativePlayer(const VPMacOSNativePlayer&) = delete;
  VPMacOSNativePlayer& operator=(const VPMacOSNativePlayer&) = delete;

  // Lock order for code that needs both native player state and presentation
  // callback state is mutex -> callback_mutex. Presentation-target helpers may
  // take the locks in separate phases, but must not nest callback_mutex before
  // mutex.
  bool renderer_active_locked() const;
  void shutdown_renderer_locked();
  void clear_last_frame_locked();
  bool ensure_renderer_locked(std::string& error);
  void on_frame_available(const vr::PresentationBackendFrameInfo* frame_info);
  void on_frame_failed(const char* error);
  void on_renderer_event(const vr::RendererEvent& event);
  void on_playback_frame_ready(const vr::RendererEvent& event);
  void record_presentation_failure_locked(const std::string& error,
                                          bool upload_failure);
  void update_decode_names_locked();
  VPMacOSNativeTrackInfo track_info_for_file_id_locked(int file_id);

  std::mutex mutex;
  std::unique_ptr<vr::Renderer> renderer;
  std::string opened_path;
  bool use_hardware_decode = true;
  float background_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  std::atomic<bool> renderer_active{false};
  std::string decode_mode_name = "none";
  std::string decoder_name = "none";
  std::chrono::steady_clock::time_point perf_start_time =
      std::chrono::steady_clock::now();

  mutable std::mutex callback_mutex;
  std::condition_variable presentation_condition;
  std::condition_variable callback_condition;
  VPMacOSFrameAvailableCallback frame_available_callback = nullptr;
  void* frame_available_user_data = nullptr;
  uint64_t frame_available_callback_generation = 0;
  uint64_t frame_available_callback_in_flight = 0;
  uint64_t manual_refresh_callback_suppression_count = 0;
  VPMacOSMetalPresentationBackend* presentation_target_backend = nullptr;
  void* presentation_target_pixel_buffer = nullptr;
  std::vector<void*> presentation_target_pixel_buffers;
  int32_t presentation_target_width = 0;
  int32_t presentation_target_height = 0;
  int32_t presentation_target_max_track_slots = 1;
  uint64_t presentation_target_generation = 0;
  bool last_native_target_presentation_succeeded = false;
  bool last_native_target_frame_info_available = false;
  VPMacOSNativeFrameInfo last_native_target_frame_info = {};
  uint64_t last_native_target_layout_revision = 0;
  uint64_t native_target_presentation_upload_count = 0;
  uint64_t native_target_presentation_failure_count = 0;
  uint64_t native_target_presentation_draw_failure_count = 0;
  uint64_t native_target_presentation_event_sequence = 0;
  uint64_t native_target_presentation_consecutive_failures = 0;
  std::vector<uint64_t> native_target_presentation_upload_intervals_ns;
  uint64_t native_target_presentation_upload_interval_p95_ms = 0;
  uint64_t native_target_warmup_generation = 0;
  uint64_t native_target_warmup_remaining = 0;
  uint64_t native_target_warmup_sample_count = 0;
  uint64_t native_target_warmup_last_ms = 0;
  uint64_t native_target_warmup_p95_ms = 0;
  std::vector<uint64_t> native_target_warmup_intervals_ns;
  std::string native_target_presentation_last_error;
  int64_t native_target_refresh_min_pts_us = -1;
  std::chrono::steady_clock::time_point native_target_presentation_first_upload_time{};
  std::chrono::steady_clock::time_point native_target_presentation_last_upload_time{};
};

#endif  // VOIDPLAYER_MACOS_NATIVE_PLAYER_STATE_H_
