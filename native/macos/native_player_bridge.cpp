#include "native_player_bridge.h"

#include "macos/native_player_state.h"
#include "macos/presentation_adapter.h"
#include "video_renderer/capture/bgra_capture_metrics.h"
#include "video_renderer/layout/layout_geometry.h"
#include "video_renderer/render/shader_constants.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>

using vp_macos::probe_videotoolbox_h264;
using vp_macos::to_layout_state;
using vp_macos::to_native_layout_state;
using vp_macos::videotoolbox_hwdownload_forced_by_env;
using vp_macos::write_error;

VPMacOSNativePlayer* VPMacOSNativePlayerCreate(void) {
  return new (std::nothrow) VPMacOSNativePlayer();
}

void VPMacOSNativePlayerDestroy(VPMacOSNativePlayer* player) {
  delete player;
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
  const int slot = player->renderer->add_track_with_file_id(path ? path : "", file_id);
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
  write_error(error, error_size, "");
  return 0;
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
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    const bool target_changed =
        player->presentation_target_backend != backend ||
        player->presentation_target_pixel_buffer != pixel_buffer ||
        player->presentation_target_width != width ||
        player->presentation_target_height != height ||
        player->presentation_target_max_track_slots !=
            std::clamp(max_track_slots, static_cast<int32_t>(1),
                       static_cast<int32_t>(VPMacOSNativeMaxTracks));
    player->presentation_target_backend = backend;
    player->presentation_target_pixel_buffer = pixel_buffer;
    player->presentation_target_width = width;
    player->presentation_target_height = height;
    player->presentation_target_max_track_slots =
        std::clamp(max_track_slots, static_cast<int32_t>(1),
                   static_cast<int32_t>(VPMacOSNativeMaxTracks));
    if (target_changed) {
      player->last_renderer_owned_presentation_succeeded = false;
      player->last_renderer_owned_frame_info_available = false;
      player->last_renderer_owned_frame_info = {};
    }
  }

  std::lock_guard<std::mutex> player_lock(player->mutex);
  if (player->renderer_active_locked()) {
    return player->renderer->update_headless_output(
               pixel_buffer,
               width,
               height,
               std::clamp(max_track_slots,
                          static_cast<int32_t>(1),
                          static_cast<int32_t>(VPMacOSNativeMaxTracks)))
               ? 0
               : -1;
  }
  if (!player->opened_path.empty()) {
    std::string message;
    if (!player->ensure_renderer_locked(message)) {
      return -1;
    }
  }
  return 0;
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
    player->last_renderer_owned_presentation_succeeded = false;
    player->last_renderer_owned_frame_info_available = false;
    player->last_renderer_owned_frame_info = {};
  }
  std::lock_guard<std::mutex> player_lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->clear_headless_output();
  }
}

int VPMacOSNativePlayerRendererOwnedPresentationActive(VPMacOSNativePlayer* player) {
  return player && player->renderer_active.load(std::memory_order_acquire) ? 1 : 0;
}

int VPMacOSNativePlayerLastRendererOwnedPresentationSucceeded(
    VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  return player->last_renderer_owned_presentation_succeeded ? 1 : 0;
}

int VPMacOSNativePlayerCopyLastRendererOwnedFrameInfo(
    VPMacOSNativePlayer* player,
    VPMacOSNativeFrameInfo* out) {
  if (!player || !out) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  if (!player->last_renderer_owned_frame_info_available) {
    return -1;
  }
  *out = player->last_renderer_owned_frame_info;
  return 0;
}

void VPMacOSNativePlayerResetRendererOwnedPresentationStats(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  player->last_renderer_owned_presentation_succeeded = false;
  player->last_renderer_owned_frame_info_available = false;
  player->last_renderer_owned_frame_info = {};
  player->renderer_owned_presentation_upload_count = 0;
  player->renderer_owned_presentation_failure_count = 0;
  player->renderer_owned_presentation_first_upload_time = {};
  player->renderer_owned_presentation_last_upload_time = {};
}

uint64_t VPMacOSNativePlayerRendererOwnedPresentationUploadCount(
    VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  return player->renderer_owned_presentation_upload_count;
}

uint64_t VPMacOSNativePlayerRendererOwnedPresentationFailureCount(
    VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  return player->renderer_owned_presentation_failure_count;
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

void VPMacOSNativePlayerPlay(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->play();
  }
}

void VPMacOSNativePlayerPause(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->pause();
  }
}

void VPMacOSNativePlayerSetSpeed(VPMacOSNativePlayer* player, double speed) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->set_speed(speed);
  }
}

void VPMacOSNativePlayerSetLoopRange(VPMacOSNativePlayer* player,
                                     int enabled,
                                     int64_t start_us,
                                     int64_t end_us) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->set_loop_range(enabled != 0, start_us, end_us);
  }
}

void VPMacOSNativePlayerSetAudibleTrack(VPMacOSNativePlayer* player,
                                        int32_t file_id) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->set_audible_track(file_id);
  }
}

void VPMacOSNativePlayerSetTrackOffset(VPMacOSNativePlayer* player,
                                       int32_t file_id,
                                       int64_t offset_us) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->set_track_offset(file_id, offset_us);
  }
}

int64_t VPMacOSNativePlayerTrackOffsetUs(VPMacOSNativePlayer* player,
                                         int32_t file_id) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked()
      ? player->renderer->track_offset_us(file_id)
      : 0;
}

void VPMacOSNativePlayerApplyLayout(VPMacOSNativePlayer* player,
                                    const VPMacOSNativeLayoutState* state) {
  if (!player || !state) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->clear_last_frame_locked();
    player->renderer->apply_layout(to_layout_state(*state));
  }
}

int VPMacOSNativePlayerCopyLayout(VPMacOSNativePlayer* player,
                                  VPMacOSNativeLayoutState* out) {
  if (!player || !out) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (!player->renderer_active_locked()) {
    *out = {};
    return 0;
  }
  *out = to_native_layout_state(player->renderer->layout());
  return 0;
}

int VPMacOSNativePlayerCopyLayoutPresentationParams(
    VPMacOSNativePlayer* player,
    int32_t width,
    int32_t height,
    VPMacOSNativeLayoutPresentationParams* out) {
  if (!player || !out || width <= 0 || height <= 0) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (!player->renderer_active_locked()) {
    return -1;
  }
  vr::LayoutTrackGeometryList tracks = {};
  const auto infos = player->renderer->track_infos();
  for (const auto& info : infos) {
    if (info.slot < 0 || info.slot >= static_cast<int>(tracks.size())) {
      continue;
    }
    const float aspect = info.height > 0
        ? static_cast<float>(info.width) / static_cast<float>(info.height)
        : 1.0f;
    tracks[static_cast<size_t>(info.slot)] = {true, info.width, info.height, aspect};
  }
  vr::ShaderConstants constants = {};
  vr::populate_layout_shader_constants(
      constants, player->renderer->layout(), tracks, width, height);
  out->display_offset_x = constants.display_offset_x[0];
  out->display_offset_y = constants.display_offset_y[0];
  out->inv_display_size_x = constants.inv_display_size_x[0];
  out->inv_display_size_y = constants.inv_display_size_y[0];
  out->view_offset_uv_x = constants.view_offset_uv_x[0];
  out->view_offset_uv_y = constants.view_offset_uv_y[0];
  return 0;
}

void VPMacOSNativePlayerSeek(VPMacOSNativePlayer* player, int64_t pts_us) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->clear_last_frame_locked();
    player->renderer->seek(pts_us, vr::SeekType::Exact);
  }
}

int VPMacOSNativePlayerStepForward(VPMacOSNativePlayer* player,
                                   char* error,
                                   size_t error_size) {
  if (!player) {
    write_error(error, error_size, "player is null");
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (!player->renderer_active_locked()) {
    write_error(error, error_size, "player is not open");
    return -1;
  }
  player->clear_last_frame_locked();
  player->renderer->step_forward();
  write_error(error, error_size, "");
  return 0;
}

int VPMacOSNativePlayerStepBackward(VPMacOSNativePlayer* player,
                                    char* error,
                                    size_t error_size) {
  if (!player) {
    write_error(error, error_size, "player is null");
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (!player->renderer_active_locked()) {
    write_error(error, error_size, "player is not open");
    return -1;
  }
  player->clear_last_frame_locked();
  player->renderer->step_backward();
  write_error(error, error_size, "");
  return 0;
}

int64_t VPMacOSNativePlayerCurrentPtsUs(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() ? player->renderer->current_pts_us() : 0;
}

int64_t VPMacOSNativePlayerDurationUs(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() ? player->renderer->duration_us() : 0;
}

int32_t VPMacOSNativePlayerWidth(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked()
      ? player->renderer->track_dimensions(0).first
      : 0;
}

int32_t VPMacOSNativePlayerHeight(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked()
      ? player->renderer->track_dimensions(0).second
      : 0;
}

int VPMacOSNativePlayerIsPlaying(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() && player->renderer->is_playing() ? 1 : 0;
}

int VPMacOSNativePlayerHasAudio(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() && player->renderer->has_audio() ? 1 : 0;
}

int32_t VPMacOSNativePlayerAudioSampleRate(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() ? player->renderer->audio_sample_rate() : 0;
}

int32_t VPMacOSNativePlayerAudioChannels(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() ? player->renderer->audio_channels() : 0;
}

int32_t VPMacOSNativePlayerActiveAudioTrack(VPMacOSNativePlayer* player) {
  if (!player) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() ? player->renderer->audible_track() : -1;
}

int VPMacOSNativePlayerHardwareDecodeActive(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->update_decode_names_locked();
  return player->decode_mode_name == "shared-renderer-videotoolbox" ? 1 : 0;
}

int VPMacOSNativePlayerHardwareDecodeDownloadsToCpu(VPMacOSNativePlayer* player) {
  return player && videotoolbox_hwdownload_forced_by_env() ? 1 : 0;
}

const char* VPMacOSNativePlayerDecodeModeName(VPMacOSNativePlayer* player) {
  if (!player) {
    return "none";
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->update_decode_names_locked();
  return player->decode_mode_name.c_str();
}

const char* VPMacOSNativePlayerDecoderName(VPMacOSNativePlayer* player) {
  if (!player) {
    return "none";
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->update_decode_names_locked();
  return player->decoder_name.c_str();
}

const char* VPMacOSNativePresentationAdapterName(void) {
  return vp_macos::presentation_adapter_name();
}

const char* VPMacOSNativePresentationSchedulerName(void) {
  return "shared-renderer";
}

int VPMacOSNativePlayerCopyPresentationSchedulerStats(
    VPMacOSNativePlayer* player,
    VPMacOSNativePresentationSchedulerStats* out) {
  if (!player || !out) {
    return -1;
  }
  *out = {};
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  out->tick_count = player->renderer_owned_presentation_upload_count;
  out->presentable_tick_count = player->renderer_owned_presentation_upload_count;
  out->frame_notification_count = player->renderer_owned_presentation_upload_count;
  out->last_selected_pts_us = player->last_renderer_owned_frame_info.pts_us;
  out->last_present_frame_count = player->last_renderer_owned_frame_info_available ? 1 : 0;
  out->cached_present_decision_available =
      player->last_renderer_owned_frame_info_available ? 1 : 0;
  out->deadline_sleep_count = 0;
  out->last_deadline_sleep_us = 0;
  return 0;
}

int VPMacOSNativePlayerCopyPerfStats(
    VPMacOSNativePlayer* player,
    VPMacOSNativePlayerPerfStats* out) {
  if (!player || !out) {
    return -1;
  }
  *out = {};
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    out->decode_elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - player->perf_start_time)
            .count();
    if (player->renderer_active_locked()) {
      const auto stats = player->renderer->track_perf_stats();
      if (!stats.empty()) {
        out->decode_frame_count = stats.front().frames_decoded;
        out->decode_fps = stats.front().fps;
        out->decode_avg_ms = stats.front().avg_decode_ms;
        out->decode_max_ms = stats.front().max_decode_ms;
      }
      const auto backend_stats = player->renderer->presentation_backend_stats();
      out->renderer_owned_direct_yuv_upload_count =
          backend_stats.direct_yuv_upload_count;
      out->renderer_owned_cvpixelbuffer_upload_count =
          backend_stats.cvpixelbuffer_upload_count;
      out->renderer_owned_present_package_upload_count =
          backend_stats.present_package_upload_count;
      out->renderer_owned_present_package_copy_us =
          backend_stats.last_present_package_copy_us;
      out->renderer_owned_present_package_gpu_wait_us =
          backend_stats.last_present_package_gpu_wait_us;
      out->renderer_owned_present_package_total_us =
          backend_stats.last_present_package_total_us;
      out->renderer_owned_present_package_storage =
          backend_stats.last_present_package_storage;
    }
  }
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    out->renderer_owned_upload_count =
        player->renderer_owned_presentation_upload_count;
    out->renderer_owned_upload_failure_count =
        player->renderer_owned_presentation_failure_count;
    if (player->renderer_owned_presentation_upload_count > 1 &&
        player->renderer_owned_presentation_last_upload_time >=
            player->renderer_owned_presentation_first_upload_time) {
      out->renderer_owned_upload_elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              player->renderer_owned_presentation_last_upload_time -
              player->renderer_owned_presentation_first_upload_time)
              .count();
      if (out->renderer_owned_upload_elapsed_ms > 0) {
        out->renderer_owned_upload_fps =
            static_cast<double>(player->renderer_owned_presentation_upload_count - 1) *
            1000.0 /
            static_cast<double>(out->renderer_owned_upload_elapsed_ms);
      }
    }
  }
  return 0;
}

int VPMacOSNativeHardwareDecodeAvailable(void) {
  static const bool available = probe_videotoolbox_h264();
  return available ? 1 : 0;
}

const char* VPMacOSNativeHardwareDecodeProviderName(void) {
  return VPMacOSNativeHardwareDecodeAvailable() != 0 ? "VideoToolbox" : "none";
}

int VPMacOSMeasureBGRA(const uint8_t* bgra,
                       int32_t width,
                       int32_t height,
                       int32_t stride_bytes,
                       VPMacOSCaptureMetrics* out) {
  if (!out) {
    return -1;
  }
  *out = {};
  const auto metrics =
      vr::measure_bgra_capture(bgra, width, height, stride_bytes);
  out->width = metrics.width;
  out->height = metrics.height;
  out->avg_luma = metrics.avg_luma;
  out->non_black_ratio = metrics.non_black_ratio;
  out->hash = metrics.hash;
  return bgra && width > 0 && height > 0 && stride_bytes >= width * 4 ? 0 : -1;
}
