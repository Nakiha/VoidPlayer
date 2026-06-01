#include "native_player_bridge.h"

#include "common/logging.h"
#include "macos/native_player_state.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

#include <spdlog/spdlog.h>

using vp_macos::write_error;

VPMacOSNativePlayer* VPMacOSNativePlayerCreate(void) {
  return new (std::nothrow) VPMacOSNativePlayer();
}

void VPMacOSNativePlayerDestroy(VPMacOSNativePlayer* player) {
  delete player;
}

void VPMacOSConfigureLogging(const char* logs_dir,
                             const char* log_file_name,
                             const char* level) {
  if (!logs_dir || logs_dir[0] == '\0') {
    return;
  }
  const std::string dir(logs_dir);
  const std::string file_name =
      log_file_name && log_file_name[0] != '\0' ? log_file_name : "native_main.log";
  const std::string level_name =
      level && level[0] != '\0' ? level : "info";

  vr::LogConfig config;
  config.file_path = dir + "/" + file_name;
  config.level = spdlog::level::from_str(level_name);
  config.max_files = 5;
  config.use_environment_level_override = true;
  config.manage_global_flush = true;
  vr::configure_logging(config);
  spdlog::info("[MacOSNative] native logging configured: {}", config.file_path);
}

void VPMacOSLogProfilerSummary(const char* message) {
  if (!message || message[0] == '\0') {
    return;
  }
  spdlog::info("[MacOSProfilerSummary] {}", message);
}

int VPMacOSNativePlayerOpen(VPMacOSNativePlayer* player,
                            const char* path,
                            char* error,
                            size_t error_size) {
  if (!player) {
    write_error(error, error_size, "player is null");
    return -1;
  }
  if (!path || std::strlen(path) == 0) {
    write_error(error, error_size, "path is empty");
    return -1;
  }

  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->shutdown_renderer_locked();
  }
  player->opened_path = path;
  std::string message;
  if (player->presentation_target_pixel_buffer &&
      !player->ensure_renderer_locked(message)) {
    write_error(error, error_size, message);
    return -1;
  }
  write_error(error, error_size, "");
  return 0;
}

int VPMacOSNativePlayerAddTrack(VPMacOSNativePlayer* player,
                                const char* path,
                                int32_t file_id,
                                int use_hardware_decode,
                                VPMacOSNativeTrackInfo* out,
                                char* error,
                                size_t error_size) {
  if (!player || !out) {
    write_error(error, error_size, "player or output track info is null");
    return -1;
  }
  *out = {};
  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (!player->ensure_renderer_locked(message)) {
    write_error(error, error_size, message);
    return -1;
  }
  const int slot = player->renderer->add_track_with_file_id(
      path ? path : "",
      file_id,
      use_hardware_decode != 0 && !vp_macos::videotoolbox_disabled_by_env());
  if (slot < 0) {
    write_error(error, error_size, "shared macOS renderer failed to add track");
    return -1;
  }
  *out = player->track_info_for_file_id_locked(file_id);
  if (out->slot < 0) {
    write_error(error, error_size, "shared macOS renderer did not report added track");
    return -1;
  }
  player->update_decode_names_locked();
  {
    std::lock_guard<std::mutex> callback_lock(player->callback_mutex);
    player->renderer_owned_refresh_min_pts_us =
        std::max<int64_t>(0, player->renderer->current_pts_us() - 500'000);
  }
  write_error(error, error_size, "");
  return 0;
}

int VPMacOSNativePlayerCopyTrackInfo(VPMacOSNativePlayer* player,
                                     int32_t file_id,
                                     VPMacOSNativeTrackInfo* out) {
  if (!player || !out) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  *out = player->track_info_for_file_id_locked(file_id);
  return out->slot >= 0 ? 0 : -1;
}

void VPMacOSNativePlayerRemoveTrack(VPMacOSNativePlayer* player,
                                    int32_t file_id) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->remove_track(file_id);
  }
}

void VPMacOSNativePlayerClose(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->shutdown_renderer_locked();
}

void VPMacOSNativePlayerSetHardwareDecodeEnabled(VPMacOSNativePlayer* player,
                                                 int enabled) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->use_hardware_decode = enabled != 0;
}

void VPMacOSNativePlayerSetBackgroundColor(VPMacOSNativePlayer* player,
                                           float r,
                                           float g,
                                           float b,
                                           float a) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->background_color[0] = std::clamp(r, 0.0f, 1.0f);
  player->background_color[1] = std::clamp(g, 0.0f, 1.0f);
  player->background_color[2] = std::clamp(b, 0.0f, 1.0f);
  player->background_color[3] = std::clamp(a, 0.0f, 1.0f);
  if (player->renderer_active_locked()) {
    player->renderer->set_background_color(player->background_color[0],
                                           player->background_color[1],
                                           player->background_color[2],
                                           player->background_color[3]);
  }
}
