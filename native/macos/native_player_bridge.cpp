#include "native_player_bridge.h"

#include "macos/presentation_adapter.h"
#include "video_renderer/capture/bgra_capture_metrics.h"
#include "video_renderer/decode/hw/hw_decode_provider.h"
#include "video_renderer/layout/layout_geometry.h"
#include "video_renderer/render/shader_constants.h"
#include "video_renderer/renderer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
}

namespace {

void write_error(char* error, size_t error_size, const std::string& message) {
  if (!error || error_size == 0) {
    return;
  }
  const size_t copy_size = std::min(error_size - 1, message.size());
  std::memcpy(error, message.data(), copy_size);
  error[copy_size] = '\0';
}

bool env_enabled(const char* name) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return false;
  }
  return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0;
}

bool videotoolbox_disabled_by_env() {
  return env_enabled("VOIDPLAYER_DISABLE_VIDEOTOOLBOX");
}

bool videotoolbox_hwdownload_forced_by_env() {
  return env_enabled("VOIDPLAYER_FORCE_VIDEOTOOLBOX_HWDOWNLOAD");
}

bool probe_videotoolbox_h264() {
  if (videotoolbox_disabled_by_env()) {
    return false;
  }
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    return false;
  }

  vr::HwDecodeInitParams params;
  params.backend = vr::RenderBackendType::Metal;
  params.device_mode = videotoolbox_hwdownload_forced_by_env()
      ? vr::DecodeDeviceMode::FfmpegOwnedHwDownloadDevice
      : vr::DecodeDeviceMode::IndependentDevice;
  params.width = 320;
  params.height = 180;
  auto result = vr::try_hw_decode_providers(codec, params);
  const bool available =
      result.success &&
      result.type == vr::HwDecodeType::VideoToolbox &&
      result.hw_device_ctx;
  if (result.hw_device_ctx) {
    av_buffer_unref(&result.hw_device_ctx);
  }
  if (result.provider) {
    result.provider->shutdown();
  }
  return available;
}

vr::LayoutState to_layout_state(const VPMacOSNativeLayoutState& state) {
  vr::LayoutState layout;
  layout.mode = state.mode;
  layout.split_pos = state.split_pos;
  layout.zoom_ratio = state.zoom_ratio;
  layout.view_offset[0] = state.view_offset_x;
  layout.view_offset[1] = state.view_offset_y;
  layout.pixel_size_mode = state.pixel_size_mode;
  for (int i = 0; i < 4; ++i) {
    layout.order[i] = state.order[i];
  }
  return layout;
}

VPMacOSNativeLayoutState to_native_layout_state(const vr::LayoutState& layout) {
  VPMacOSNativeLayoutState state = {};
  state.mode = layout.mode;
  state.split_pos = layout.split_pos;
  state.zoom_ratio = layout.zoom_ratio;
  state.view_offset_x = layout.view_offset[0];
  state.view_offset_y = layout.view_offset[1];
  state.pixel_size_mode = layout.pixel_size_mode;
  for (int i = 0; i < 4; ++i) {
    state.order[i] = layout.order[i];
  }
  return state;
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool decoder_name_is_videotoolbox(const std::string& decoder_name) {
  return lower_ascii(decoder_name).find("videotoolbox") != std::string::npos;
}

}  // namespace

struct VPMacOSNativePlayer {
  VPMacOSNativePlayer() = default;
  ~VPMacOSNativePlayer() { shutdown_renderer_locked(); }

  VPMacOSNativePlayer(const VPMacOSNativePlayer&) = delete;
  VPMacOSNativePlayer& operator=(const VPMacOSNativePlayer&) = delete;

  bool renderer_active_locked() const {
    return renderer && renderer->is_initialized();
  }

  void shutdown_renderer_locked() {
    if (renderer) {
      renderer->shutdown();
      renderer.reset();
    }
    opened_path.clear();
    decode_mode_name = "none";
    decoder_name = "none";
    renderer_active.store(false, std::memory_order_release);
    clear_last_frame_locked();
  }

  void clear_last_frame_locked() {
    std::lock_guard<std::mutex> callback_lock(callback_mutex);
    last_renderer_owned_presentation_succeeded = false;
    last_renderer_owned_frame_info_available = false;
    last_renderer_owned_frame_info = {};
  }

  bool ensure_renderer_locked(std::string& error) {
    if (renderer_active_locked()) {
      return true;
    }
    if (opened_path.empty()) {
      error = "path is empty";
      return false;
    }

    void* output = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    int32_t max_track_slots = 1;
    {
      std::lock_guard<std::mutex> callback_lock(callback_mutex);
      output = presentation_target_pixel_buffer;
      width = presentation_target_width;
      height = presentation_target_height;
      max_track_slots = presentation_target_max_track_slots;
    }
    if (!output || width <= 0 || height <= 0) {
      error = "renderer-owned Metal presentation target is not installed";
      return false;
    }

    auto next_renderer = std::make_unique<vr::Renderer>();
    vr::RendererConfig config;
    config.video_paths = {opened_path};
    config.width = width;
    config.height = height;
    config.headless = true;
    config.use_hardware_decode = !videotoolbox_disabled_by_env();
    config.initial_file_id = 0;
    config.backend.type = vr::RendererBackendType::Metal;
    config.backend.output = output;
    config.backend.max_track_slots =
        std::clamp(max_track_slots,
                   static_cast<int32_t>(1),
                   static_cast<int32_t>(VPMacOSNativeMaxTracks));
    if (!next_renderer->initialize(config)) {
      error = "shared macOS renderer failed to initialize";
      return false;
    }

    renderer = std::move(next_renderer);
    renderer->set_frame_callback([this]() { on_frame_available(); });
    renderer_active.store(true, std::memory_order_release);
    perf_start_time = std::chrono::steady_clock::now();
    update_decode_names_locked();
    return true;
  }

  void on_frame_available() {
    VPMacOSFrameAvailableCallback callback = nullptr;
    void* user_data = nullptr;
    {
      std::lock_guard<std::mutex> callback_lock(callback_mutex);
      last_renderer_owned_presentation_succeeded = true;
      last_renderer_owned_frame_info_available = true;
      last_renderer_owned_frame_info.width = presentation_target_width;
      last_renderer_owned_frame_info.height = presentation_target_height;
      if (renderer) {
        last_renderer_owned_frame_info.pts_us = renderer->current_pts_us();
      }
      const auto now = std::chrono::steady_clock::now();
      if (renderer_owned_presentation_upload_count == 0) {
        renderer_owned_presentation_first_upload_time = now;
      }
      renderer_owned_presentation_last_upload_time = now;
      ++renderer_owned_presentation_upload_count;
      callback = frame_available_callback;
      user_data = frame_available_user_data;
    }
    if (callback) {
      callback(user_data);
    }
  }

  void update_decode_names_locked() {
    decode_mode_name = "none";
    decoder_name = "none";
    if (!renderer_active_locked()) {
      return;
    }
    const auto infos = renderer->track_infos();
    if (infos.empty()) {
      return;
    }
    decoder_name = infos.front().decoder_name.empty()
        ? "renderer"
        : infos.front().decoder_name;
    decode_mode_name = decoder_name_is_videotoolbox(decoder_name)
        ? "shared-renderer-videotoolbox"
        : "shared-renderer-software";
  }

  VPMacOSNativeTrackInfo track_info_for_file_id_locked(int file_id) {
    VPMacOSNativeTrackInfo out = {};
    if (!renderer_active_locked()) {
      return out;
    }
    const auto infos = renderer->track_infos();
    for (const auto& info : infos) {
      if (info.file_id != file_id) {
        continue;
      }
      out.file_id = info.file_id;
      out.slot = info.slot;
      out.width = info.width;
      out.height = info.height;
      out.duration_us = info.duration_us;
      return out;
    }
    return out;
  }

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
    player->presentation_target_backend = backend;
    player->presentation_target_pixel_buffer = pixel_buffer;
    player->presentation_target_width = width;
    player->presentation_target_height = height;
    player->presentation_target_max_track_slots =
        std::clamp(max_track_slots, static_cast<int32_t>(1),
                   static_cast<int32_t>(VPMacOSNativeMaxTracks));
    player->last_renderer_owned_presentation_succeeded = false;
    player->last_renderer_owned_frame_info_available = false;
    player->last_renderer_owned_frame_info = {};
  }

  std::lock_guard<std::mutex> player_lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->resize(width, height);
    return 0;
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
    player->renderer->shutdown();
    player->renderer.reset();
    player->renderer_active.store(false, std::memory_order_release);
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
        out->decode_fps = stats.front().fps;
        out->decode_avg_ms = stats.front().avg_decode_ms;
        out->decode_max_ms = stats.front().max_decode_ms;
      }
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
