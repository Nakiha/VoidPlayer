#include "native_player_bridge.h"

#include "common/logging.h"
#include "macos/player/native_player_state.h"
#include "renderer/decode/frame_color_metadata.h"

#if VOID_BUILD_ANALYSIS
#include "analysis/analysis_manager.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
}

using vp_macos::write_error;

namespace {

void fill_codec_names(VPMacOSNativeTrackInfo* out, AVCodecID codec_id) {
  if (!out) {
    return;
  }
  const char* codec_name = avcodec_get_name(codec_id);
  vp_macos::write_error(
      out->codec_name,
      sizeof(out->codec_name),
      codec_name ? codec_name : "");
  const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codec_id);
  vp_macos::write_error(
      out->codec_long_name,
      sizeof(out->codec_long_name),
      descriptor && descriptor->long_name ? descriptor->long_name : "");
}

void fill_track_info_from_stream(VPMacOSNativeTrackInfo* out,
                                 AVFormatContext* format_context,
                                 AVStream* stream) {
  if (!out || !format_context || !stream || !stream->codecpar) {
    return;
  }
  const AVCodecParameters* params = stream->codecpar;
  out->width = params->width;
  out->height = params->height;
  if (stream->start_time != AV_NOPTS_VALUE) {
    out->start_time_us =
        av_rescale_q(stream->start_time, stream->time_base, {1, 1000000});
  } else if (format_context->start_time != AV_NOPTS_VALUE) {
    out->start_time_us =
        av_rescale_q(format_context->start_time, {1, AV_TIME_BASE}, {1, 1000000});
  }
  if (format_context->duration != AV_NOPTS_VALUE) {
    out->duration_us =
        av_rescale_q(format_context->duration, {1, AV_TIME_BASE}, {1, 1000000});
  }
  out->bit_rate = params->bit_rate > 0 ? params->bit_rate : format_context->bit_rate;
  if (format_context->iformat) {
    vp_macos::write_error(
        out->format_name,
        sizeof(out->format_name),
        format_context->iformat->long_name
            ? format_context->iformat->long_name
            : (format_context->iformat->name ? format_context->iformat->name : ""));
  }
  fill_codec_names(out, params->codec_id);
  const auto color = vr::color_info_from_av_codec_parameters(params);
  out->color_range = color.range;
  out->color_matrix = color.matrix;
  out->color_transfer = color.transfer;
  out->color_primaries = color.primaries;
}

}  // namespace

VPMacOSNativePlayer* VPMacOSNativePlayerCreate(void) {
  return new (std::nothrow) VPMacOSNativePlayer();
}

void VPMacOSNativePlayerDestroy(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  VPMacOSNativePlayerSetFrameAvailableCallback(player, nullptr, nullptr);
  VPMacOSNativePlayerSetSourceCacheFrameAvailableCallback(
      player, nullptr, nullptr);
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

void VPMacOSNativeAnalysisOverlayClearTracks(void) {
#if VOID_BUILD_ANALYSIS
  vr::analysis::AnalysisManager::instance().clear_overlay_tracks();
#endif
}

int VPMacOSNativeAnalysisOverlaySetTrack(int32_t track_file_id,
                                         const char* analysis_path) {
#if VOID_BUILD_ANALYSIS
  if (track_file_id < 0 || !analysis_path || analysis_path[0] == '\0') {
    return 0;
  }
  return vr::analysis::AnalysisManager::instance().set_overlay_track(
             track_file_id,
             analysis_path)
      ? 1
      : 0;
#else
  (void)track_file_id;
  (void)analysis_path;
  return 0;
#endif
}

void VPMacOSNativeAnalysisOverlaySetState(int32_t show_cu_grid,
                                          int32_t show_pred_mode,
                                          int32_t show_qp_heatmap,
                                          int32_t show_pred_lines,
                                          int32_t show_cu_bit_cost_heatmap,
                                          int32_t opacity_permille,
                                          int32_t mode,
                                          int32_t track_file_id) {
#if VOID_BUILD_ANALYSIS
  auto& overlay = vr::analysis::AnalysisManager::instance().overlay;
  overlay.show_cu_grid.store(show_cu_grid != 0, std::memory_order_release);
  overlay.show_pred_mode.store(show_pred_mode != 0, std::memory_order_release);
  overlay.show_qp_heatmap.store(show_qp_heatmap != 0, std::memory_order_release);
  overlay.show_pred_lines.store(show_pred_lines != 0, std::memory_order_release);
  overlay.show_cu_bit_cost_heatmap.store(show_cu_bit_cost_heatmap != 0,
                                         std::memory_order_release);
  overlay.opacity_permille.store(std::clamp(opacity_permille, 0, 1000),
                                 std::memory_order_release);
  overlay.mode.store(std::max(0, mode), std::memory_order_release);
  overlay.track_file_id.store(track_file_id, std::memory_order_release);
#else
  (void)show_cu_grid;
  (void)show_pred_mode;
  (void)show_qp_heatmap;
  (void)show_pred_lines;
  (void)show_cu_bit_cost_heatmap;
  (void)opacity_permille;
  (void)mode;
  (void)track_file_id;
#endif
}

int VPMacOSNativeProbeTrackInfo(const char* path,
                                VPMacOSNativeTrackInfo* out,
                                char* error,
                                size_t error_size) {
  if (!out) {
    write_error(error, error_size, "output track info is null");
    return -1;
  }
  *out = {};
  out->slot = -1;
  if (!path || std::strlen(path) == 0) {
    write_error(error, error_size, "path is empty");
    return -1;
  }

  AVFormatContext* format_context = nullptr;
  if (avformat_open_input(&format_context, path, nullptr, nullptr) < 0) {
    write_error(error, error_size, "failed to open input");
    return -1;
  }
  auto close_input = [&]() {
    avformat_close_input(&format_context);
  };

  if (avformat_find_stream_info(format_context, nullptr) < 0) {
    close_input();
    write_error(error, error_size, "failed to read stream info");
    return -1;
  }

  int video_stream_index = -1;
  for (unsigned int i = 0; i < format_context->nb_streams; ++i) {
    AVStream* stream = format_context->streams[i];
    if (stream && stream->codecpar &&
        stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_index = static_cast<int>(i);
      break;
    }
  }
  if (video_stream_index < 0) {
    close_input();
    write_error(error, error_size, "no video stream");
    return -1;
  }

  out->file_id = 0;
  out->slot = 0;
  fill_track_info_from_stream(
      out, format_context, format_context->streams[video_stream_index]);
  close_input();
  write_error(error, error_size, "");
  return 0;
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
  bool target_installed = false;
  {
    std::lock_guard<std::mutex> callback_lock(player->callback_mutex);
    target_installed =
        player->presentation_target_pixel_buffer &&
        player->presentation_target_width > 0 &&
        player->presentation_target_height > 0;
  }
  std::string message;
  if (target_installed && !player->ensure_renderer_locked(message)) {
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
  const bool allow_hardware_decode =
      use_hardware_decode != 0 &&
      !vp_macos::videotoolbox_disabled_by_env();
  const int slot = player->renderer->add_track_with_file_id(
      path ? path : "",
      file_id,
      allow_hardware_decode);
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
  const int64_t current_pts_us = player->renderer->current_pts_us();
  {
    std::lock_guard<std::mutex> callback_lock(player->callback_mutex);
    player->renderer_owned_refresh_min_pts_us =
        std::max<int64_t>(0, current_pts_us - 500'000);
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
