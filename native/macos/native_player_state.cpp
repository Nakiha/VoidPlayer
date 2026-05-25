#include "macos/native_player_state.h"

#include "video_renderer/decode/hw/hw_decode_provider.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
}

namespace vp_macos {

void write_error(char* error, size_t error_size, const std::string& message) {
  if (!error || error_size == 0) {
    return;
  }
  const size_t copy_size = std::min(error_size - 1, message.size());
  std::memcpy(error, message.data(), copy_size);
  error[copy_size] = '\0';
}

namespace {

bool env_enabled(const char* name) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return false;
  }
  return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0;
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

}  // namespace

bool decoder_name_is_videotoolbox(const std::string& decoder_name) {
  return lower_ascii(decoder_name).find("videotoolbox") != std::string::npos;
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

}  // namespace vp_macos

VPMacOSNativePlayer::~VPMacOSNativePlayer() {
  shutdown_renderer_locked();
}

bool VPMacOSNativePlayer::renderer_active_locked() const {
  return renderer && renderer->is_initialized();
}

void VPMacOSNativePlayer::shutdown_renderer_locked() {
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

void VPMacOSNativePlayer::clear_last_frame_locked() {
  std::lock_guard<std::mutex> callback_lock(callback_mutex);
  last_renderer_owned_presentation_succeeded = false;
  last_renderer_owned_frame_info_available = false;
  last_renderer_owned_frame_info = {};
}

bool VPMacOSNativePlayer::ensure_renderer_locked(std::string& error) {
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
  config.use_hardware_decode = !vp_macos::videotoolbox_disabled_by_env();
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
  renderer->set_frame_failure_callback(
      [this](const char* message) { on_frame_failed(message); });
  renderer_active.store(true, std::memory_order_release);
  perf_start_time = std::chrono::steady_clock::now();
  update_decode_names_locked();
  return true;
}

void VPMacOSNativePlayer::on_frame_available() {
  VPMacOSFrameAvailableCallback callback = nullptr;
  void* user_data = nullptr;
  {
    std::lock_guard<std::mutex> callback_lock(callback_mutex);
    last_renderer_owned_presentation_succeeded = true;
    last_renderer_owned_frame_info_available = true;
    renderer_owned_presentation_consecutive_failures = 0;
    renderer_owned_presentation_last_error.clear();
    last_renderer_owned_frame_info.width = presentation_target_width;
    last_renderer_owned_frame_info.height = presentation_target_height;
    if (renderer) {
      vr::PresentationBackendFrameInfo frame_info;
      if (renderer->copy_last_presentation_frame_info(&frame_info)) {
        last_renderer_owned_frame_info.width = frame_info.width;
        last_renderer_owned_frame_info.height = frame_info.height;
        last_renderer_owned_frame_info.pts_us = frame_info.pts_us;
        last_renderer_owned_frame_info.dts_us = frame_info.dts_us;
        last_renderer_owned_frame_info.duration_us = frame_info.duration_us;
      } else {
        last_renderer_owned_frame_info.pts_us = renderer->current_pts_us();
      }
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

void VPMacOSNativePlayer::record_presentation_failure_locked(
    const std::string& error,
    bool upload_failure) {
  last_renderer_owned_presentation_succeeded = false;
  last_renderer_owned_frame_info_available = false;
  ++renderer_owned_presentation_draw_failure_count;
  if (upload_failure) {
    ++renderer_owned_presentation_failure_count;
  }
  ++renderer_owned_presentation_consecutive_failures;
  renderer_owned_presentation_last_error =
      error.empty() ? "renderer-owned Metal presentation failed" : error;
}

void VPMacOSNativePlayer::on_frame_failed(const char* error) {
  std::lock_guard<std::mutex> callback_lock(callback_mutex);
  record_presentation_failure_locked(
      error ? std::string(error) : std::string(), false);
}

void VPMacOSNativePlayer::update_decode_names_locked() {
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
  decode_mode_name = vp_macos::decoder_name_is_videotoolbox(decoder_name)
      ? "shared-renderer-videotoolbox"
      : "shared-renderer-software";
}

VPMacOSNativeTrackInfo VPMacOSNativePlayer::track_info_for_file_id_locked(
    int file_id) {
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
