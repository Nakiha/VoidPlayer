#include "macos/player/native_player_state.h"

#include "media/ffmpeg_lifetime.h"
#include "renderer/decode/hw/hw_decode_provider.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <spdlog/spdlog.h>
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

constexpr uint64_t kRendererOwnedIntervalSampleLimit = 256;
constexpr uint64_t kTargetWarmupMaxIntervalNs = 250ull * 1000ull * 1000ull;

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

uint64_t percentile_95_ms(std::vector<uint64_t> samples_ns) {
  if (samples_ns.empty()) {
    return 0;
  }
  std::sort(samples_ns.begin(), samples_ns.end());
  const size_t index = std::min(
      samples_ns.size() - 1,
      (samples_ns.size() * 95 + 99) / 100 - 1);
  return samples_ns[index] / (1000ull * 1000ull);
}

void append_interval_sample(std::vector<uint64_t>& samples_ns,
                            uint64_t interval_ns) {
  samples_ns.push_back(interval_ns);
  if (samples_ns.size() > kRendererOwnedIntervalSampleLimit) {
    samples_ns.erase(samples_ns.begin(),
                     samples_ns.begin() +
                         (samples_ns.size() - kRendererOwnedIntervalSampleLimit));
  }
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
  vr::AvBufferRefOwner hw_device_ctx(result.hw_device_ctx);
  result.hw_device_ctx = nullptr;
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
  std::lock_guard<std::mutex> lock(mutex);
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
  last_renderer_owned_layout_revision = 0;
  renderer_owned_refresh_min_pts_us = -1;
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
  config.use_hardware_decode =
      use_hardware_decode && !vp_macos::videotoolbox_disabled_by_env();
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
  next_renderer->set_background_color(background_color[0],
                                      background_color[1],
                                      background_color[2],
                                      background_color[3]);

  renderer = std::move(next_renderer);
  renderer->set_frame_callback(
      [this](const vr::PresentationBackendFrameInfo* frame_info) {
        on_frame_available(frame_info);
      });
  renderer->set_frame_failure_callback(
      [this](const char* message) { on_frame_failed(message); });
  renderer_active.store(true, std::memory_order_release);
  perf_start_time = std::chrono::steady_clock::now();
  update_decode_names_locked();
  return true;
}

void VPMacOSNativePlayer::on_frame_available(
    const vr::PresentationBackendFrameInfo* completed_frame_info) {
  const auto start = std::chrono::steady_clock::now();
  VPMacOSFrameAvailableCallback callback = nullptr;
  void* user_data = nullptr;
  bool callback_in_flight = false;
  bool suppress_external_callback = false;
  uint64_t upload_count = 0;
  int64_t pts_us = -1;
  {
    std::lock_guard<std::mutex> callback_lock(callback_mutex);
    last_renderer_owned_presentation_succeeded = true;
    last_renderer_owned_frame_info_available = true;
    renderer_owned_presentation_consecutive_failures = 0;
    renderer_owned_presentation_last_error.clear();
    VPMacOSNativeFrameInfoInit(&last_renderer_owned_frame_info);
    last_renderer_owned_frame_info.width = presentation_target_width;
    last_renderer_owned_frame_info.height = presentation_target_height;
    if (completed_frame_info) {
      last_renderer_owned_frame_info.width = completed_frame_info->width;
      last_renderer_owned_frame_info.height = completed_frame_info->height;
      last_renderer_owned_frame_info.pts_us = completed_frame_info->pts_us;
      last_renderer_owned_frame_info.dts_us = completed_frame_info->dts_us;
      last_renderer_owned_frame_info.duration_us =
          completed_frame_info->duration_us;
      last_renderer_owned_frame_info.analysis_frame_index =
          completed_frame_info->analysis_frame_index;
      last_renderer_owned_frame_info.frame_identity_mode =
          completed_frame_info->frame_identity_mode;
      last_renderer_owned_frame_info.source_packet_index =
          completed_frame_info->source_packet_index;
      last_renderer_owned_frame_info.source_packet_size =
          completed_frame_info->source_packet_size;
      last_renderer_owned_frame_info.source_packet_pos =
          completed_frame_info->source_packet_pos;
      last_renderer_owned_frame_info.source_packet_pts =
          completed_frame_info->source_packet_pts;
      last_renderer_owned_frame_info.source_packet_dts =
          completed_frame_info->source_packet_dts;
      last_renderer_owned_frame_info.color_range =
          completed_frame_info->color_range;
      last_renderer_owned_frame_info.color_matrix =
          completed_frame_info->color_matrix;
      last_renderer_owned_frame_info.color_transfer =
          completed_frame_info->color_transfer;
      last_renderer_owned_frame_info.color_primaries =
          completed_frame_info->color_primaries;
      last_renderer_owned_frame_info.target_pixel_buffer_address =
          completed_frame_info->target_pixel_buffer_address;
      last_renderer_owned_frame_info.layout_revision =
          completed_frame_info->layout_revision;
      last_renderer_owned_layout_revision = completed_frame_info->layout_revision;
    }
    const auto now = std::chrono::steady_clock::now();
    if (renderer_owned_presentation_upload_count > 0 &&
        renderer_owned_presentation_last_upload_time.time_since_epoch().count() != 0) {
      const auto interval_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              now - renderer_owned_presentation_last_upload_time)
              .count());
      vp_macos::append_interval_sample(
          renderer_owned_presentation_upload_intervals_ns, interval_ns);
      renderer_owned_presentation_upload_interval_p95_ms =
          vp_macos::percentile_95_ms(
              renderer_owned_presentation_upload_intervals_ns);
      if (renderer_owned_target_warmup_remaining > 0 &&
          interval_ns <= vp_macos::kTargetWarmupMaxIntervalNs) {
        vp_macos::append_interval_sample(
            renderer_owned_target_warmup_intervals_ns, interval_ns);
        --renderer_owned_target_warmup_remaining;
        ++renderer_owned_target_warmup_sample_count;
        renderer_owned_target_warmup_last_ms =
            interval_ns / (1000ull * 1000ull);
        renderer_owned_target_warmup_p95_ms =
            vp_macos::percentile_95_ms(
                renderer_owned_target_warmup_intervals_ns);
      }
    }
    if (renderer_owned_presentation_upload_count == 0) {
      renderer_owned_presentation_first_upload_time = now;
    }
    renderer_owned_presentation_last_upload_time = now;
    ++renderer_owned_presentation_upload_count;
    ++renderer_owned_presentation_event_sequence;
    upload_count = renderer_owned_presentation_upload_count;
    pts_us = last_renderer_owned_frame_info.pts_us;
    if (manual_refresh_callback_suppression_count > 0) {
      --manual_refresh_callback_suppression_count;
      suppress_external_callback = true;
    }
    if (suppress_external_callback && vp_macos::env_enabled("VOIDPLAYER_MACOS_PROFILER")) {
      spdlog::info(
          "[MacOSFrameRefresh] frame_available pts_us={} upload_count={} layout_revision={} "
          "target_buffer=0x{:x}",
          pts_us,
          upload_count,
          last_renderer_owned_layout_revision,
          last_renderer_owned_frame_info.target_pixel_buffer_address);
    }
    if (!suppress_external_callback) {
      callback = frame_available_callback;
      user_data = frame_available_user_data;
    }
    if (callback) {
      ++frame_available_callback_in_flight;
      callback_in_flight = true;
    }
  }
  presentation_condition.notify_all();
  if (callback) {
    callback(user_data);
  }
  if (callback_in_flight) {
    {
      std::lock_guard<std::mutex> callback_lock(callback_mutex);
      if (frame_available_callback_in_flight > 0) {
        --frame_available_callback_in_flight;
      }
    }
    callback_condition.notify_all();
  }
  const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start).count();
  if (vp_macos::env_enabled("VOIDPLAYER_MACOS_PROFILER") &&
      (elapsed_us >= 2000 || upload_count % 240 == 0)) {
    spdlog::info(
        "[MacOSProfiler] frame_available total_us={} upload_count={} pts_us={} has_callback={} suppressed_manual_refresh={}",
        elapsed_us,
        upload_count,
        pts_us,
        callback != nullptr,
        suppress_external_callback);
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
  ++renderer_owned_presentation_event_sequence;
  ++renderer_owned_presentation_consecutive_failures;
  renderer_owned_presentation_last_error =
      error.empty() ? "renderer-owned Metal presentation failed" : error;
}

void VPMacOSNativePlayer::on_frame_failed(const char* error) {
  uint64_t failure_count = 0;
  int suppressed_refresh_count = 0;
  std::string message = error ? std::string(error) : std::string();
  {
    std::lock_guard<std::mutex> callback_lock(callback_mutex);
    if (manual_refresh_callback_suppression_count > 0) {
      --manual_refresh_callback_suppression_count;
      suppressed_refresh_count = 1;
    }
    record_presentation_failure_locked(message, false);
    failure_count = renderer_owned_presentation_draw_failure_count;
    message = renderer_owned_presentation_last_error;
  }
  spdlog::warn(
      "[MacOSFrameRefresh] frame_failed failures={} suppressed_manual_refresh={} error={}",
      failure_count,
      suppressed_refresh_count,
      message);
  presentation_condition.notify_all();
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
    out.start_time_us = info.start_time_us;
    out.bit_rate = info.bit_rate;
    vp_macos::write_error(
        out.format_name, sizeof(out.format_name), info.format_name);
    vp_macos::write_error(
        out.codec_name, sizeof(out.codec_name), info.codec_name);
    vp_macos::write_error(
        out.codec_long_name, sizeof(out.codec_long_name), info.codec_long_name);
    vp_macos::write_error(
        out.decoder_name, sizeof(out.decoder_name), info.decoder_name);
    out.color_range = info.color.range;
    out.color_matrix = info.color.matrix;
    out.color_transfer = info.color.transfer;
    out.color_primaries = info.color.primaries;
    return out;
  }
  return out;
}
