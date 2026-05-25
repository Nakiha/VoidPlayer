#include "native_player_bridge.h"

#include "macos/native_player_state.h"

#include <algorithm>
#include <mutex>
#include <string>

using vp_macos::write_error;

void VPMacOSNativePlayerSetFrameAvailableCallback(
    VPMacOSNativePlayer* player,
    VPMacOSFrameAvailableCallback callback,
    void* user_data) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  player->frame_available_callback = callback;
  player->frame_available_user_data = user_data;
}

int VPMacOSNativePlayerSetMetalPresentationTarget(
    VPMacOSNativePlayer* player,
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots) {
  if (!player || !pixel_buffer || width <= 0 || height <= 0) {
    return -1;
  }
  const int32_t clamped_track_slots =
      std::clamp(max_track_slots, static_cast<int32_t>(1),
                 static_cast<int32_t>(VPMacOSNativeMaxTracks));
  bool target_changed = false;
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    target_changed =
        player->presentation_target_backend != backend ||
        player->presentation_target_pixel_buffer != pixel_buffer ||
        player->presentation_target_width != width ||
        player->presentation_target_height != height ||
        player->presentation_target_max_track_slots != clamped_track_slots;
    player->presentation_target_backend = backend;
    player->presentation_target_pixel_buffer = pixel_buffer;
    player->presentation_target_width = width;
    player->presentation_target_height = height;
    player->presentation_target_max_track_slots = clamped_track_slots;
    if (target_changed) {
      ++player->presentation_target_generation;
      player->last_renderer_owned_presentation_succeeded = false;
      player->last_renderer_owned_frame_info_available = false;
      player->last_renderer_owned_frame_info = {};
      player->renderer_owned_presentation_consecutive_failures = 0;
      player->renderer_owned_presentation_last_error.clear();
    }
  }

  std::string renderer_error;
  {
    std::lock_guard<std::mutex> player_lock(player->mutex);
    if (player->renderer_active_locked()) {
      if (!target_changed) {
        return 0;
      }
      if (player->renderer->update_headless_output(
              pixel_buffer, width, height, clamped_track_slots)) {
        return 0;
      }
      renderer_error = "failed to install renderer-owned Metal presentation target";
    } else if (!player->opened_path.empty()) {
      if (player->ensure_renderer_locked(renderer_error)) {
        return 0;
      }
    } else {
      return 0;
    }
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  player->record_presentation_failure_locked(renderer_error, true);
  return -1;
}

void VPMacOSNativePlayerClearMetalPresentationTarget(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    player->presentation_target_backend = nullptr;
    player->presentation_target_pixel_buffer = nullptr;
    player->presentation_target_width = 0;
    player->presentation_target_height = 0;
    player->presentation_target_max_track_slots = 1;
    ++player->presentation_target_generation;
    player->record_presentation_failure_locked(
        "renderer-owned Metal presentation target was cleared", false);
  }
  std::lock_guard<std::mutex> player_lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->clear_headless_output();
  }
}

int VPMacOSNativePlayerPresentCurrentFrameToMetalTarget(
    VPMacOSNativePlayer* player,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  if (!player || !out) {
    write_error(error, error_size, "player or renderer-owned frame output is null");
    return -1;
  }
  *out = {};
  std::string message;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    if (!player->ensure_renderer_locked(message)) {
      write_error(error, error_size, message);
      return -1;
    }
  }
  std::lock_guard<std::mutex> callback_lock(player->callback_mutex);
  if (!player->last_renderer_owned_frame_info_available) {
    write_error(error, error_size, "shared macOS renderer has not presented a frame yet");
    return -1;
  }
  *out = player->last_renderer_owned_frame_info;
  write_error(error, error_size, "");
  return 0;
}
