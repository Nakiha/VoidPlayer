#ifndef VOIDPLAYER_MACOS_NATIVE_PLAYER_STATE_H_
#define VOIDPLAYER_MACOS_NATIVE_PLAYER_STATE_H_

#include "macos/metal_presentation_backend_bridge.h"
#include "macos/native_player_types.h"
#include "video_renderer/renderer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace vp_macos {

void write_error(char* error, size_t error_size, const std::string& message);
bool videotoolbox_disabled_by_env();
bool videotoolbox_hwdownload_forced_by_env();
bool probe_videotoolbox_h264();
vr::LayoutState to_layout_state(const VPMacOSNativeLayoutState& state);
VPMacOSNativeLayoutState to_native_layout_state(const vr::LayoutState& layout);

}  // namespace vp_macos

struct VPMacOSNativePlayer {
  VPMacOSNativePlayer() = default;
  ~VPMacOSNativePlayer();

  VPMacOSNativePlayer(const VPMacOSNativePlayer&) = delete;
  VPMacOSNativePlayer& operator=(const VPMacOSNativePlayer&) = delete;

  bool renderer_active_locked() const;
  void shutdown_renderer_locked();
  void clear_last_frame_locked();
  bool ensure_renderer_locked(std::string& error);
  void on_frame_available();
  void update_decode_names_locked();
  VPMacOSNativeTrackInfo track_info_for_file_id_locked(int file_id);

  std::mutex mutex;
  std::unique_ptr<vr::Renderer> renderer;
  std::string opened_path;
  std::atomic<bool> renderer_active{false};
  std::string decode_mode_name = "none";
  std::string decoder_name = "none";
  std::chrono::steady_clock::time_point perf_start_time =
      std::chrono::steady_clock::now();

  mutable std::mutex callback_mutex;
  VPMacOSFrameAvailableCallback frame_available_callback = nullptr;
  void* frame_available_user_data = nullptr;
  VPMacOSMetalPresentationBackend* presentation_target_backend = nullptr;
  void* presentation_target_pixel_buffer = nullptr;
  int32_t presentation_target_width = 0;
  int32_t presentation_target_height = 0;
  int32_t presentation_target_max_track_slots = 1;
  bool last_renderer_owned_presentation_succeeded = false;
  bool last_renderer_owned_frame_info_available = false;
  VPMacOSNativeFrameInfo last_renderer_owned_frame_info = {};
  uint64_t renderer_owned_presentation_upload_count = 0;
  uint64_t renderer_owned_presentation_failure_count = 0;
  std::chrono::steady_clock::time_point renderer_owned_presentation_first_upload_time{};
  std::chrono::steady_clock::time_point renderer_owned_presentation_last_upload_time{};
};

#endif  // VOIDPLAYER_MACOS_NATIVE_PLAYER_STATE_H_
