#include "native_player_bridge.h"

#include "macos/native_player_state.h"

#include <algorithm>
#include <chrono>
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
  if (target_changed) {
    player->presentation_condition.notify_all();
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
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    player->record_presentation_failure_locked(renderer_error, true);
  }
  player->presentation_condition.notify_all();
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
  player->presentation_condition.notify_all();
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

int VPMacOSNativePlayerRequestRendererOwnedFrameRefresh(
    VPMacOSNativePlayer* player,
    int32_t timeout_ms,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  if (!player || !out) {
    write_error(error, error_size, "player or renderer-owned frame output is null");
    return -1;
  }
  *out = {};
  const int32_t bounded_timeout_ms = std::max<int32_t>(0, timeout_ms);

  uint64_t baseline_upload_count = 0;
  uint64_t baseline_draw_failure_count = 0;
  uint64_t baseline_target_generation = 0;
  bool baseline_frame_available = false;
  int64_t refresh_clock_us = 0;
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    if (!player->presentation_target_pixel_buffer ||
        player->presentation_target_width <= 0 ||
        player->presentation_target_height <= 0) {
      write_error(error, error_size,
                  "renderer-owned Metal presentation target is not installed");
      return -1;
    }
    baseline_upload_count = player->renderer_owned_presentation_upload_count;
    baseline_draw_failure_count =
        player->renderer_owned_presentation_draw_failure_count;
    baseline_target_generation = player->presentation_target_generation;
    baseline_frame_available = player->last_renderer_owned_frame_info_available;
  }

  std::string message;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    if (!player->ensure_renderer_locked(message)) {
      write_error(error, error_size, message);
      return -1;
    }
    if (player->renderer) {
      refresh_clock_us = player->renderer->current_pts_us();
      player->renderer->request_frame_refresh("macos-renderer-owned-refresh");
    }
  }

  std::unique_lock<std::mutex> callback_lock(player->callback_mutex);
  const bool enforce_refresh_pts_window =
      !baseline_frame_available && refresh_clock_us > 0;
  const auto frame_matches_refresh_request = [&]() {
    if (!player->last_renderer_owned_frame_info_available) {
      return false;
    }
    if (!enforce_refresh_pts_window) {
      return true;
    }
    constexpr int64_t kRefreshPtsLowerToleranceUs = 500'000;
    constexpr int64_t kRefreshPtsUpperToleranceUs = 1'500'000;
    const int64_t pts_us = player->last_renderer_owned_frame_info.pts_us;
    return pts_us >= refresh_clock_us - kRefreshPtsLowerToleranceUs &&
           pts_us <= refresh_clock_us + kRefreshPtsUpperToleranceUs;
  };
  const auto completed = [&]() {
    return player->presentation_target_generation != baseline_target_generation ||
           (player->renderer_owned_presentation_upload_count >
                baseline_upload_count &&
            frame_matches_refresh_request()) ||
           player->renderer_owned_presentation_draw_failure_count >
               baseline_draw_failure_count;
  };
  if (bounded_timeout_ms > 0) {
    player->presentation_condition.wait_for(
        callback_lock,
        std::chrono::milliseconds(bounded_timeout_ms),
        completed);
  }

  if (player->presentation_target_generation != baseline_target_generation) {
    write_error(error, error_size,
                "renderer-owned Metal presentation target changed during refresh");
    return -1;
  }
  if (player->renderer_owned_presentation_upload_count > baseline_upload_count &&
      frame_matches_refresh_request()) {
    *out = player->last_renderer_owned_frame_info;
    write_error(error, error_size, "");
    return 0;
  }
  if (player->renderer_owned_presentation_draw_failure_count >
      baseline_draw_failure_count) {
    write_error(error, error_size,
                player->renderer_owned_presentation_last_error.empty()
                    ? "renderer-owned Metal frame refresh failed"
                    : player->renderer_owned_presentation_last_error);
    return -1;
  }
  write_error(error, error_size,
              "renderer-owned Metal frame refresh timed out");
  return -2;
}
