#include "native_player_bridge.h"

#include "macos/presentation_adapter.h"
#include "audio/audio_output.h"
#include "audio/audio_output_factory.h"
#include "media/demux_thread.h"
#include "media/packet_queue.h"
#include "media/seek_controller.h"
#include "playback/playback_controller.h"
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/decode/decode_thread.h"
#include "video_renderer/decode/hw/hw_decode_provider.h"
#include "video_renderer/layout/layout_controller.h"
#include "video_renderer/layout/layout_geometry.h"
#include "video_renderer/render/presentation_package.h"
#include "video_renderer/render/render_loop_controller.h"
#include "video_renderer/render/presentation_scheduler.h"
#include "video_renderer/render/presentation_snapshot.h"
#include "video_renderer/render/shader_constants.h"
#include "video_renderer/capture/bgra_capture_metrics.h"
#include "video_renderer/seek/seek_coordinator.h"
#include "video_renderer/sync/render_sink.h"
#include "video_renderer/track/track_pipeline_factory.h"

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>

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

class ScopedCVPixelBufferLock {
public:
  explicit ScopedCVPixelBufferLock(CVPixelBufferRef buffer)
      : buffer_(buffer),
        locked_(buffer &&
                CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) ==
                    kCVReturnSuccess) {}

  ~ScopedCVPixelBufferLock() {
    if (locked_) {
      CVPixelBufferUnlockBaseAddress(buffer_, kCVPixelBufferLock_ReadOnly);
    }
  }

  ScopedCVPixelBufferLock(const ScopedCVPixelBufferLock&) = delete;
  ScopedCVPixelBufferLock& operator=(const ScopedCVPixelBufferLock&) = delete;

  bool locked() const { return locked_; }

private:
  CVPixelBufferRef buffer_ = nullptr;
  bool locked_ = false;
};

bool videotoolbox_disabled_by_env() {
  const char* value = std::getenv("VOIDPLAYER_DISABLE_VIDEOTOOLBOX");
  if (!value || value[0] == '\0') {
    return false;
  }
  return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0;
}

bool videotoolbox_hwdownload_forced_by_env() {
  const char* value = std::getenv("VOIDPLAYER_FORCE_VIDEOTOOLBOX_HWDOWNLOAD");
  if (!value || value[0] == '\0') {
    return false;
  }
  return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0;
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

size_t align_up_size(size_t value, size_t alignment) {
  if (alignment == 0) {
    return value;
  }
  const size_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
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

class MacOSNativePlayerCore {
public:
  MacOSNativePlayerCore()
      : playback_(vr::create_default_audio_output) {}

  ~MacOSNativePlayerCore() { close(); }

  bool open(const char* path, std::string& error) {
    close();
    if (!path || std::strlen(path) == 0) {
      error = "path is empty";
      return false;
    }

    render_sink_ = std::make_unique<vr::RenderSink>(playback_.clock());
    layout_controller_.reset(layout_);

    VPMacOSNativeTrackInfo track_info = {};
    if (!add_track_locked(path, 0, track_info, error, true)) {
      close();
      return false;
    }
    width_ = track_info.width;
    height_ = track_info.height;

    playback_.seek_clock(0);
    playback_.pause();
    playing_ = false;
    presentation_scheduler_.reset();
    reset_scheduler_stats();
    clear_scheduler_present_decision();
    return true;
  }

  void close() {
    playing_ = false;
    playback_.stop_session();
    for (auto& track : tracks_) {
      stop_track(track);
      track.reset();
    }
    render_sink_.reset();
    width_ = 0;
    height_ = 0;
    duration_us_ = 0;
    audio_available_ = false;
    audio_sample_rate_ = 0;
    audio_channels_ = 0;
    loop_range_ = vr::LoopRangeState();
    layout_controller_.reset(layout_);
    presentation_scheduler_.reset();
    reset_scheduler_stats();
    clear_scheduler_present_decision();
  }

  void play() {
    if (!find_track_by_file_id(0)) {
      return;
    }
    playing_ = true;
    playback_.play();
  }

  void pause() {
    playing_ = false;
    playback_.pause();
  }

  void set_speed(double speed) {
    playback_.set_speed(std::max(0.01, speed));
  }

  void set_loop_range(bool enabled, int64_t start_us, int64_t end_us) {
    const auto next = vr::normalize_loop_range_state(enabled, start_us, end_us);
    if (vr::loop_range_states_equal(loop_range_, next)) {
      return;
    }
    loop_range_ = next;
  }

  void set_audible_track(int32_t file_id) {
    if (auto* audio = playback_.audio_output()) {
      audio->set_active_track(file_id);
    }
  }

  void set_track_offset(int32_t file_id, int64_t offset_us) {
    auto* track = find_track_by_file_id(file_id);
    if (!track || !render_sink_) {
      return;
    }
    track->offset_us = offset_us;
    render_sink_->set_track_offset(static_cast<size_t>(track->slot), offset_us);
    clear_scheduler_present_decision();
  }

  int64_t track_offset_us(int32_t file_id) const {
    const auto* track = find_track_by_file_id(file_id);
    return track ? track->offset_us : 0;
  }

  void apply_layout(const VPMacOSNativeLayoutState& state) {
    const auto requested = to_layout_state(state);
    layout_controller_.apply(layout_, requested, [this](int file_id) {
      return slot_for_file_id(file_id);
    });
  }

  VPMacOSNativeLayoutState layout_snapshot() const {
    return to_native_layout_state(layout_controller_.snapshot(layout_));
  }

  bool layout_presentation_params(int32_t width,
                                  int32_t height,
                                  VPMacOSNativeLayoutPresentationParams* out) const {
    if (!out || width <= 0 || height <= 0 || width_ <= 0 || height_ <= 0) {
      return false;
    }
    vr::LayoutTrackGeometryList tracks = {};
    for (const auto& track : tracks_) {
      if (!track || track->slot < 0 || track->slot >= static_cast<int32_t>(tracks.size())) {
        continue;
      }
      const float aspect = track->video_height > 0
          ? static_cast<float>(track->video_width) / static_cast<float>(track->video_height)
          : 1.0f;
      tracks[static_cast<size_t>(track->slot)] = {
          true,
          track->video_width,
          track->video_height,
          aspect,
      };
    }
    vr::ShaderConstants constants = {};
    vr::populate_layout_shader_constants(constants, layout_, tracks, width, height);
    out->display_offset_x = constants.display_offset_x[0];
    out->display_offset_y = constants.display_offset_y[0];
    out->inv_display_size_x = constants.inv_display_size_x[0];
    out->inv_display_size_y = constants.inv_display_size_y[0];
    out->view_offset_uv_x = constants.view_offset_uv_x[0];
    out->view_offset_uv_y = constants.view_offset_uv_y[0];
    return true;
  }

  bool copy_present_frames_into(uint8_t* dst,
                                size_t dst_size,
                                int32_t width,
                                int32_t height,
                                int32_t stride_bytes,
                                size_t track_stride_bytes,
                                VPMacOSNativePresentDecisionInfo* out,
                                std::string& error) {
    if (!dst || !out || width <= 0 || height <= 0 || stride_bytes < width * 4) {
      error = "invalid present decision BGRA destination";
      return false;
    }
    if (!render_sink_) {
      error = "player is not open";
      return false;
    }

    const auto decision = current_present_decision();
    fill_present_decision_info(decision, width, height, out);
    if (!decision.should_present) {
      error = "no presentable frame is ready";
      return false;
    }
    if (out->track_count > 1 && out->frame_count < out->track_count) {
      error = "not all present decision frames are ready";
      return false;
    }
    size_t required_tracks = 1;
    for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
      if (decision.frames[slot].has_value()) {
        required_tracks = std::max(required_tracks, slot + 1);
      }
    }
    const size_t min_track_stride =
        static_cast<size_t>(stride_bytes) * static_cast<size_t>(height);
    if (track_stride_bytes < min_track_stride ||
        track_stride_bytes > std::numeric_limits<size_t>::max() / required_tracks ||
        dst_size < track_stride_bytes * required_tracks) {
      error = "present decision BGRA destination is too small";
      return false;
    }

    for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
      if (!decision.frames[slot].has_value()) {
        continue;
      }
      auto* slot_dst = dst + track_stride_bytes * slot;
      const auto& frame = *decision.frames[slot];
      VPMacOSNativeFrameInfo frame_info = {};
      const auto status = vp_macos::copy_texture_frame_to_bgra_destination_checked(
          frame,
          slot_dst,
          track_stride_bytes,
          frame.width,
          frame.height,
          stride_bytes,
          &frame_info);
      if (status != vp_macos::PresentationAdapterStatus::Ok) {
        error = vp_macos::presentation_adapter_status_message(status);
        return false;
      }
    }
    return true;
  }

  bool copy_present_frames_yuv_into(uint8_t* dst,
                                    size_t dst_size,
                                    int32_t width,
                                    int32_t height,
                                    size_t max_track_slots,
                                    VPMacOSNativePresentDecisionInfo* out,
                                    std::string& error) {
    if (!dst || !out || width <= 0 || height <= 0 || max_track_slots == 0) {
      error = "invalid present decision YUV destination";
      return false;
    }
    if (!render_sink_) {
      error = "player is not open";
      return false;
    }

    const auto decision = current_present_decision();
    fill_present_decision_info(decision, width, height, out);
    if (!decision.should_present) {
      error = "no presentable frame is ready";
      return false;
    }
    if (out->track_count > 1 && out->frame_count < out->track_count) {
      error = "not all present decision frames are ready";
      return false;
    }

    size_t cursor = 0;
    for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
      if (!decision.frames[slot].has_value()) {
        continue;
      }
      if (slot >= max_track_slots) {
        error = "present decision YUV destination is too small";
        return false;
      }
      const auto& frame = *decision.frames[slot];
      const auto* storage = frame.cpu_nv12_storage();
      const auto* cv_storage = frame.macos_cv_pixel_buffer_storage();
      const uint8_t* y_source = nullptr;
      const uint8_t* uv_source = nullptr;
      int y_stride = 0;
      int uv_stride = 0;
      int coded_width = 0;
      int coded_height = 0;
      bool is_p010 = false;
      std::unique_ptr<ScopedCVPixelBufferLock> cv_lock;
      if (storage) {
        if (!storage->data || storage->y_stride <= 0 || storage->uv_stride <= 0 ||
            storage->coded_width <= 0 || storage->coded_height <= 0 ||
            storage->coded_width < frame.width ||
            storage->coded_height < frame.height) {
          error = "present decision contains invalid NV12 frame storage";
          return false;
        }
        y_source = storage->data->data();
        y_stride = storage->y_stride;
        uv_stride = storage->uv_stride;
        coded_width = storage->coded_width;
        coded_height = storage->coded_height;
        is_p010 = storage->is_p010;
        uv_source = y_source + static_cast<size_t>(y_stride) * coded_height;
      } else if (cv_storage) {
        auto* pixel_buffer = static_cast<CVPixelBufferRef>(cv_storage->pixel_buffer);
        cv_lock = std::make_unique<ScopedCVPixelBufferLock>(pixel_buffer);
        if (!pixel_buffer || !cv_lock->locked() || cv_storage->plane_count < 2 ||
            cv_storage->coded_width <= 0 || cv_storage->coded_height <= 0 ||
            cv_storage->coded_width < frame.width ||
            cv_storage->coded_height < frame.height) {
          error = "present decision contains invalid CVPixelBuffer frame storage";
          return false;
        }
        y_source = static_cast<const uint8_t*>(
            CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
        uv_source = static_cast<const uint8_t*>(
            CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
        y_stride = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0));
        uv_stride = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1));
        coded_width = cv_storage->coded_width;
        coded_height = cv_storage->coded_height;
        is_p010 = cv_storage->is_p010;
      } else {
        error = "present decision contains non-NV12 frame storage";
        return false;
      }
      if (!y_source || !uv_source || y_stride <= 0 || uv_stride <= 0) {
        error = "present decision contains invalid YUV plane pointers";
        return false;
      }

      const int bytes_per_sample = is_p010 ? 2 : 1;
      if (y_stride < coded_width * bytes_per_sample ||
          uv_stride < coded_width * bytes_per_sample) {
        error = "invalid NV12/P010 frame storage for Metal presentation";
        return false;
      }
      const int chroma_height = (coded_height + 1) / 2;
      const size_t y_bytes =
          static_cast<size_t>(y_stride) * static_cast<size_t>(coded_height);
      const size_t uv_bytes =
          static_cast<size_t>(uv_stride) * static_cast<size_t>(chroma_height);
      if (storage && (y_bytes > std::numeric_limits<size_t>::max() - uv_bytes ||
          y_bytes + uv_bytes > storage->data->size())) {
        error = "invalid NV12/P010 frame storage for Metal presentation";
        return false;
      }
      cursor = align_up_size(cursor, static_cast<size_t>(bytes_per_sample));
      if (cursor > dst_size || y_bytes > dst_size - cursor ||
          uv_bytes > dst_size - cursor - y_bytes) {
        error = "present decision YUV destination is too small";
        return false;
      }

      std::memcpy(dst + cursor, y_source, y_bytes);
      out->y_offset[slot] = static_cast<int32_t>(cursor);
      cursor += y_bytes;
      std::memcpy(dst + cursor, uv_source, uv_bytes);
      out->uv_offset[slot] = static_cast<int32_t>(cursor);
      cursor += uv_bytes;
      out->yuv_format[slot] = is_p010
          ? VPMacOSNativePresentFormatP010
          : VPMacOSNativePresentFormatNV12;
      out->y_stride[slot] = y_stride;
      out->uv_stride[slot] = uv_stride;
      out->coded_width[slot] = coded_width;
      out->coded_height[slot] = coded_height;
      out->nv12_uv_scale_x[slot] =
          static_cast<float>(frame.width) / static_cast<float>(coded_width);
      out->nv12_uv_scale_y[slot] =
          static_cast<float>(frame.height) / static_cast<float>(coded_height);
    }
    return true;
  }

  bool copy_retained_cv_pixel_buffer_present_frame(
      int32_t width,
      int32_t height,
      VPMacOSNativeCVPixelBufferPresentFrame* out,
      std::string& error) {
    if (!out || width <= 0 || height <= 0) {
      error = "invalid CVPixelBuffer present frame output";
      return false;
    }
    if (!render_sink_) {
      error = "player is not open";
      return false;
    }
    *out = {};
    auto decision = current_present_decision();
    if (!decision.should_present) {
      decision = primary_peek_present_decision();
    }
    fill_present_decision_info(decision, width, height, &out->decision);
    if (!decision.should_present) {
      error = "no presentable frame is ready";
      return false;
    }
    if (out->decision.frame_count != 1) {
      error = "present decision is not a single CVPixelBuffer frame";
      return false;
    }

    for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
      if (!decision.frames[slot].has_value()) {
        continue;
      }
      if (slot != 0) {
        error = "CVPixelBuffer fast path currently requires the primary track slot";
        return false;
      }
      const auto& frame = *decision.frames[slot];
      const auto* storage = frame.macos_cv_pixel_buffer_storage();
      if (!storage || !storage->pixel_buffer || storage->plane_count < 2 ||
          storage->coded_width < frame.width || storage->coded_height < frame.height) {
        error = "present decision does not contain a supported CVPixelBuffer frame";
        return false;
      }
      CVPixelBufferRetain(static_cast<CVPixelBufferRef>(storage->pixel_buffer));
      out->pixel_buffer = storage->pixel_buffer;
      out->pixel_format = static_cast<int32_t>(storage->pixel_format);
      out->plane_count = storage->plane_count;
      out->is_p010 = storage->is_p010 ? 1 : 0;
      out->coded_width = storage->coded_width;
      out->coded_height = storage->coded_height;
      out->decision.yuv_format[slot] = storage->is_p010
          ? VPMacOSNativePresentFormatP010
          : VPMacOSNativePresentFormatNV12;
      out->decision.coded_width[slot] = storage->coded_width;
      out->decision.coded_height[slot] = storage->coded_height;
      out->decision.nv12_uv_scale_x[slot] =
          static_cast<float>(frame.width) / static_cast<float>(storage->coded_width);
      out->decision.nv12_uv_scale_y[slot] =
          static_cast<float>(frame.height) / static_cast<float>(storage->coded_height);
      return true;
    }
    error = "present decision has no CVPixelBuffer frame";
    return false;
  }

  void seek(int64_t pts_us) {
    const int64_t target = std::max<int64_t>(0, pts_us);
    for (auto& track : tracks_) {
      if (!track || !track->seek_controller || !track->track_buffer || !track->packet_queue) {
        continue;
      }
      const int64_t track_target = std::max<int64_t>(0, target - track->offset_us);
      track->track_buffer->set_state(vr::TrackState::Flushing);
      track->track_buffer->clear_frames();
      track->packet_queue->flush();
      track->seek_controller->request_seek(track_target, vr::SeekType::Exact);
    }
    auto* primary = find_track_by_file_id(0);
    if (primary && primary->audio_packet_queue) {
      primary->audio_packet_queue->flush();
    }
    playback_.seek_clock(target);
    presentation_scheduler_.reset();
    scheduler_last_selected_pts_us_ = vr::kNoTimestampUs;
    scheduler_last_present_frame_count_ = 0;
    clear_scheduler_present_decision();
  }

  bool add_track(const char* path,
                 int32_t file_id,
                 VPMacOSNativeTrackInfo& out,
                 std::string& error) {
    return add_track_locked(path, file_id, out, error, false);
  }

  void remove_track(int32_t file_id) {
    const int slot = slot_for_file_id(file_id);
    if (slot < 0) {
      return;
    }
    stop_track(tracks_[static_cast<size_t>(slot)]);
    tracks_[static_cast<size_t>(slot)].reset();
    if (render_sink_) {
      render_sink_->set_track(static_cast<size_t>(slot), nullptr, -1, 0);
    }
    layout_controller_.remove_track(layout_, file_id, [this](int candidate_file_id) {
      return slot_for_file_id(candidate_file_id);
    });
    recompute_duration();
  }

  int64_t current_pts_us() const {
    return playback_.clock().current_pts_us();
  }

  int64_t duration_us() const { return duration_us_; }
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  bool is_playing() const { return playing_; }
  bool has_audio() const { return audio_available_; }
  int32_t audio_sample_rate() const { return audio_sample_rate_; }
  int32_t audio_channels() const { return audio_channels_; }
  int32_t active_audio_track() const {
    const auto* audio = playback_.audio_output();
    return audio ? audio->active_track() : -1;
  }
  bool hardware_decode_active() const {
    const auto* primary = find_track_by_file_id(0);
    return primary && primary->decode_thread &&
        primary->decode_thread->is_hardware_decode_enabled();
  }
  bool hardware_decode_downloads_to_cpu() const {
    const auto* primary = find_track_by_file_id(0);
    return primary && primary->decode_thread &&
        primary->decode_thread->memory_stats().hardware_download_to_cpu;
  }
  const char* decode_mode_name() const {
    const auto* primary = find_track_by_file_id(0);
    if (!primary || !primary->decode_thread) {
      return "none";
    }
    if (hardware_decode_active()) {
      return hardware_decode_downloads_to_cpu()
          ? "videotoolbox-download-to-cpu"
          : "videotoolbox-renderer-owned";
    }
    return "software-fallback";
  }
  const char* decoder_name() const {
    const auto* primary = find_track_by_file_id(0);
    if (!primary || !primary->decode_thread) {
      return "none";
    }
    if (!hardware_decode_active()) {
      return "decode_thread_software";
    }
    return hardware_decode_downloads_to_cpu()
        ? "decode_thread_videotoolbox_hwdownload"
        : "decode_thread_videotoolbox_renderer_owned";
  }

  bool copy_current_frame(VPMacOSNativeFrame* out, std::string& error) {
    auto* primary = find_track_by_file_id(0);
    if (!primary || !primary->track_buffer) {
      error = "player is not open";
      return false;
    }
    advance_to_clock(nullptr);
    auto frame = primary->track_buffer->peek(0);
    if (!frame.has_value()) {
      error = "no decoded frame is ready";
      return false;
    }
    if (!vp_macos::copy_texture_frame_to_owned_bgra(*frame, out)) {
      vp_macos::free_owned_bgra_frame(out);
      error = "decoded frame storage is not supported by the macOS software presentation adapter";
      return false;
    }
    return true;
  }

  bool copy_current_frame_into(uint8_t* dst,
                               size_t dst_size,
                               int32_t width,
                               int32_t height,
                               int32_t stride_bytes,
                               VPMacOSNativeFrameInfo* out,
                               std::string& error) {
    auto* primary = find_track_by_file_id(0);
    if (!primary || !primary->track_buffer) {
      error = "player is not open";
      return false;
    }
    advance_to_clock(nullptr);
    auto frame = primary->track_buffer->peek(0);
    if (!frame.has_value()) {
      error = "no decoded frame is ready";
      return false;
    }
    const auto status = vp_macos::copy_texture_frame_to_bgra_destination_checked(
        *frame, dst, dst_size, width, height, stride_bytes, out);
    if (status != vp_macos::PresentationAdapterStatus::Ok) {
      error = vp_macos::presentation_adapter_status_message(status);
      return false;
    }
    return true;
  }

  vr::PresentationSchedulerTick tick_playback() {
    ++scheduler_tick_count_;
    tick_loop_range();
    if (!playing_) {
      return {};
    }
    if (!render_sink_) {
      return {};
    }
    auto tick = presentation_scheduler_.tick(*render_sink_);
    if (tick.has_presentable_frame) {
      scheduler_cached_present_decision_ = tick.decision;
      scheduler_cached_present_decision_available_ = true;
      ++scheduler_presentable_tick_count_;
      scheduler_last_selected_pts_us_ = tick.selected_pts_us;
      scheduler_last_present_frame_count_ = 0;
      for (const auto& frame : tick.decision.frames) {
        if (frame.has_value()) {
          ++scheduler_last_present_frame_count_;
        }
      }
    }
    if (tick.should_notify) {
      ++scheduler_frame_notification_count_;
    }
    return tick;
  }

  std::chrono::microseconds next_presentation_sleep(std::chrono::microseconds max_sleep) {
    if (!playing_ || !render_sink_ || max_sleep.count() <= 0) {
      scheduler_last_deadline_sleep_us_ = 0;
      return std::chrono::microseconds(0);
    }
    const int64_t current_pts_us = playback_.clock().current_pts_us();
    const auto next_event_pts = next_frame_event_pts_us(current_pts_us);
    if (!next_event_pts.has_value()) {
      scheduler_last_deadline_sleep_us_ = 1000;
      return std::chrono::milliseconds(1);
    }
    const auto sleep_for = render_loop_controller_.frame_deadline_sleep(
        current_pts_us,
        *next_event_pts,
        playback_.clock().speed(),
        max_sleep.count());
    scheduler_last_deadline_sleep_us_ = sleep_for.count();
    if (sleep_for.count() > 0) {
      ++scheduler_deadline_sleep_count_;
    }
    return sleep_for;
  }

  VPMacOSNativePresentationSchedulerStats scheduler_stats() const {
    VPMacOSNativePresentationSchedulerStats stats = {};
    stats.tick_count = scheduler_tick_count_;
    stats.presentable_tick_count = scheduler_presentable_tick_count_;
    stats.frame_notification_count = scheduler_frame_notification_count_;
    stats.last_selected_pts_us = scheduler_last_selected_pts_us_;
    stats.last_present_frame_count = scheduler_last_present_frame_count_;
    stats.cached_present_decision_available =
        scheduler_cached_present_decision_available_ ? 1 : 0;
    stats.deadline_sleep_count = scheduler_deadline_sleep_count_;
    stats.last_deadline_sleep_us = scheduler_last_deadline_sleep_us_;
    return stats;
  }

  void tick_loop_range() {
    auto* primary = find_track_by_file_id(0);
    if (!primary || !primary->track_buffer || !primary->seek_controller) {
      return;
    }
    vr::LoopRangeSeekInput input;
    input.playing = playing_;
    input.loop_enabled = loop_range_.enabled;
    input.clock_paused = playback_.clock().is_paused();
    input.current_pts_us = playback_.clock().current_pts_us();
    input.start_us = loop_range_.start_us;
    input.end_us = loop_range_.end_us;
    const auto decision = vr::choose_loop_range_seek(input);
    if (decision.should_seek) {
      seek(decision.target_pts_us);
    }
  }

private:
  vr::PresentDecision current_present_decision() const {
    if (scheduler_cached_present_decision_available_) {
      return scheduler_cached_present_decision_;
    }
    return render_sink_ ? render_sink_->evaluate() : vr::PresentDecision();
  }

  vr::PresentDecision primary_peek_present_decision() const {
    vr::PresentDecision decision;
    const auto* primary = find_track_by_file_id(0);
    if (!primary || !primary->track_buffer || primary->slot < 0 ||
        primary->slot >= static_cast<int32_t>(vr::kMaxTracks)) {
      return decision;
    }
    auto frame = primary->track_buffer->peek(0);
    if (!frame.has_value()) {
      return decision;
    }
    const auto slot = static_cast<size_t>(primary->slot);
    decision.should_present = true;
    decision.frames[slot] = frame;
    decision.file_ids[slot] = primary->file_id;
    decision.track_generations[slot] = primary->generation;
    decision.current_pts_us = frame->pts_us + primary->offset_us;
    return decision;
  }

  void clear_scheduler_present_decision() {
    scheduler_cached_present_decision_ = {};
    scheduler_cached_present_decision_available_ = false;
  }

  bool advance_to_clock(int64_t* frame_pts_us) {
    if (!render_sink_) {
      return false;
    }
    return presentation_scheduler_.advance_to_clock(*render_sink_, frame_pts_us);
  }

  std::optional<int64_t> next_frame_event_pts_us(int64_t current_pts_us) const {
    std::optional<int64_t> next_event_pts;
    for (const auto& track : tracks_) {
      if (!track || !track->track_buffer) {
        continue;
      }
      const auto frame = track->track_buffer->peek(0);
      if (!frame.has_value()) {
        continue;
      }
      const int64_t effective_current_pts = current_pts_us - track->offset_us;
      const int64_t event_pts = frame->pts_us > effective_current_pts
          ? frame->pts_us + track->offset_us
          : frame->pts_us + frame->duration_us + track->offset_us;
      if (!next_event_pts.has_value() || event_pts < *next_event_pts) {
        next_event_pts = event_pts;
      }
    }
    return next_event_pts;
  }

  vr::LayoutTrackGeometryList layout_track_geometry() const {
    vr::LayoutTrackGeometryList tracks = {};
    for (const auto& track : tracks_) {
      if (!track || track->slot < 0 || track->slot >= static_cast<int32_t>(tracks.size())) {
        continue;
      }
      const float aspect = track->video_height > 0
          ? static_cast<float>(track->video_width) / static_cast<float>(track->video_height)
          : 1.0f;
      tracks[static_cast<size_t>(track->slot)] = {
          true,
          track->video_width,
          track->video_height,
          aspect,
      };
    }
    return tracks;
  }

  void fill_present_decision_info(const vr::PresentDecision& decision,
                                  int32_t width,
                                  int32_t height,
                                  VPMacOSNativePresentDecisionInfo* out) const {
    *out = {};
    const auto snapshot = vr::build_presentation_snapshot(
        decision, layout_, layout_track_geometry(), width, height);
    const auto& constants = snapshot.constants;
    out->should_present = snapshot.should_present ? 1 : 0;
    out->current_pts_us = snapshot.current_pts_us;
    out->frame_count = snapshot.frame_count;
    out->track_count = constants.track_count;
    out->mode = constants.mode;
    out->split_pos = constants.split_pos;
    for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
      out->order[slot] = constants.order[slot];
      out->display_offset_x[slot] = constants.display_offset_x[slot];
      out->display_offset_y[slot] = constants.display_offset_y[slot];
      out->inv_display_size_x[slot] = constants.inv_display_size_x[slot];
      out->inv_display_size_y[slot] = constants.inv_display_size_y[slot];
      out->view_offset_uv_x[slot] = constants.view_offset_uv_x[slot];
      out->view_offset_uv_y[slot] = constants.view_offset_uv_y[slot];
      auto& frame_out = out->frames[slot];
      const auto& frame = snapshot.frames[slot];
      frame_out.file_id = frame.file_id;
      frame_out.slot = static_cast<int32_t>(slot);
      out->source_width[slot] = frame.width;
      out->source_height[slot] = frame.height;
      out->nv12_uv_scale_x[slot] = frame.nv12_uv_scale_x;
      out->nv12_uv_scale_y[slot] = frame.nv12_uv_scale_y;
      out->color_range[slot] = frame.color_range;
      out->color_matrix[slot] = frame.color_matrix;
      out->color_transfer[slot] = frame.color_transfer;
      out->color_primaries[slot] = frame.color_primaries;
      out->y_stride[slot] = frame.y_stride;
      out->uv_stride[slot] = frame.uv_stride;
      out->coded_width[slot] = frame.coded_width;
      out->coded_height[slot] = frame.coded_height;
      if (!frame.present) {
        out->nv12_uv_scale_x[slot] = 1.0f;
        out->nv12_uv_scale_y[slot] = 1.0f;
        continue;
      }
      frame_out.present = 1;
      frame_out.width = frame.width;
      frame_out.height = frame.height;
      frame_out.pts_us = frame.pts_us;
      frame_out.dts_us = frame.dts_us;
      frame_out.duration_us = frame.duration_us;
    }
  }

  std::unique_ptr<vr::RenderSink> render_sink_;
  std::array<std::unique_ptr<vr::TrackPipeline>, vr::kMaxTracks> tracks_{};
  vr::TrackPipelineFactory track_pipeline_factory_;
  vr::PlaybackController playback_;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int64_t duration_us_ = 0;
  bool audio_available_ = false;
  int32_t audio_sample_rate_ = 0;
  int32_t audio_channels_ = 0;
  vr::LoopRangeState loop_range_;
  vr::LayoutState layout_;
  vr::LayoutController layout_controller_;
  vr::PresentationScheduler presentation_scheduler_;
  vr::RenderLoopController render_loop_controller_;
  bool playing_ = false;
  uint64_t scheduler_tick_count_ = 0;
  uint64_t scheduler_presentable_tick_count_ = 0;
  uint64_t scheduler_frame_notification_count_ = 0;
  uint64_t scheduler_deadline_sleep_count_ = 0;
  int64_t scheduler_last_deadline_sleep_us_ = 0;
  int64_t scheduler_last_selected_pts_us_ = vr::kNoTimestampUs;
  int32_t scheduler_last_present_frame_count_ = 0;
  vr::PresentDecision scheduler_cached_present_decision_;
  bool scheduler_cached_present_decision_available_ = false;

  void reset_scheduler_stats() {
    scheduler_tick_count_ = 0;
    scheduler_presentable_tick_count_ = 0;
    scheduler_frame_notification_count_ = 0;
    scheduler_deadline_sleep_count_ = 0;
    scheduler_last_deadline_sleep_us_ = 0;
    scheduler_last_selected_pts_us_ = vr::kNoTimestampUs;
    scheduler_last_present_frame_count_ = 0;
    clear_scheduler_present_decision();
  }

  int first_free_slot() const {
    for (size_t i = 0; i < tracks_.size(); ++i) {
      if (!tracks_[i]) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int slot_for_file_id(int file_id) const {
    for (size_t i = 0; i < tracks_.size(); ++i) {
      if (tracks_[i] && tracks_[i]->file_id == file_id) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  vr::TrackPipeline* find_track_by_file_id(int32_t file_id) {
    const int slot = slot_for_file_id(file_id);
    return slot >= 0 ? tracks_[static_cast<size_t>(slot)].get() : nullptr;
  }

  const vr::TrackPipeline* find_track_by_file_id(int32_t file_id) const {
    const int slot = slot_for_file_id(file_id);
    return slot >= 0 ? tracks_[static_cast<size_t>(slot)].get() : nullptr;
  }

  bool add_track_locked(const char* path,
                        int32_t file_id,
                        VPMacOSNativeTrackInfo& out,
                        std::string& error,
                        bool primary) {
    if (!path || std::strlen(path) == 0) {
      error = "path is empty";
      return false;
    }
    if (slot_for_file_id(file_id) >= 0) {
      error = "file id is already open";
      return false;
    }
    const int slot = primary ? 0 : first_free_slot();
    if (slot < 0 || slot >= static_cast<int>(tracks_.size()) || tracks_[slot]) {
      error = "no free macOS native track slots";
      return false;
    }

    vr::TrackPipelineOpenOptions options;
    options.render_backend = vr::RenderBackendKind::Metal;
    options.use_default_decode_device_mode = false;
    options.decode_device_mode = videotoolbox_hwdownload_forced_by_env()
        ? vr::DecodeDeviceMode::FfmpegOwnedHwDownloadDevice
        : vr::DecodeDeviceMode::IndependentDevice;
    auto track = track_pipeline_factory_.create_opened_pipeline(
        path, !videotoolbox_disabled_by_env(), nullptr, options);
    if (!track) {
      error = "failed to open shared native track pipeline";
      return false;
    }
    track->file_id = file_id;
    track->slot = slot;
    track->generation = 1;
    const auto& stats = track->demux_thread->stats();

    if (primary) {
      duration_us_ = stats.duration_us;
      audio_sample_rate_ = stats.sample_rate;
      audio_channels_ = stats.channels;
      playback_.stop_session();
      if (stats.audio_stream_index >= 0 && stats.audio_codec_params) {
        playback_.start_session();
        auto* audio = playback_.audio_output();
        audio_available_ = audio &&
            audio->add_track(0, *track->audio_packet_queue, stats.audio_codec_params,
                             stats.audio_time_base);
        if (audio_available_) {
          audio->set_active_track(0);
          audio->set_all_decode_paused(true);
        } else {
          playback_.stop_session();
        }
      }
    }

    auto* decoder = track->decode_thread.get();
    auto* audio_output = primary ? playback_.audio_output() : nullptr;
    track->demux_thread->set_seek_callback(
        [decoder, audio_output, primary](int64_t pts_us, vr::SeekType type) {
          if (decoder) {
            decoder->notify_seek(pts_us, type);
          }
          if (primary && audio_output) {
            audio_output->notify_seek(0, pts_us, type);
          }
        });
    if (!track->demux_thread->start_thread()) {
      error = "demux thread failed to start";
      track->decode_thread->stop();
      return false;
    }

    if (render_sink_) {
      render_sink_->set_track(
          static_cast<size_t>(slot), track->track_buffer, file_id, track->generation);
    }
    clear_scheduler_present_decision();
    layout_controller_.append_track(layout_, file_id, slot);
    out.file_id = file_id;
    out.slot = slot;
    out.width = track->video_width;
    out.height = track->video_height;
    out.duration_us = track->duration_us;
    tracks_[static_cast<size_t>(slot)] = std::move(track);
    recompute_duration();
    return true;
  }

  void stop_track(std::unique_ptr<vr::TrackPipeline>& track) {
    if (!track) {
      return;
    }
    if (track->decode_thread) {
      track->decode_thread->stop();
    }
    if (track->demux_thread) {
      track->demux_thread->stop();
    }
  }

  void recompute_duration() {
    int64_t duration = 0;
    for (const auto& track : tracks_) {
      if (!track) {
        continue;
      }
      duration = std::max(duration, track->duration_us + track->offset_us);
    }
    duration_us_ = duration;
  }
};

}  // namespace

struct VPMacOSNativePlayer {
  VPMacOSNativePlayer() = default;
  ~VPMacOSNativePlayer() { stop_tick_thread(); }

  VPMacOSNativePlayer(const VPMacOSNativePlayer&) = delete;
  VPMacOSNativePlayer& operator=(const VPMacOSNativePlayer&) = delete;

  bool start_tick_thread() {
    if (tick_thread.joinable()) {
      return true;
    }
    tick_stop.store(false);
    try {
      tick_thread = std::thread([this] { run_tick_thread(); });
    } catch (...) {
      return false;
    }
    return true;
  }

  void stop_tick_thread() {
    tick_stop.store(true);
    tick_cv.notify_all();
    if (tick_thread.joinable()) {
      tick_thread.join();
    }
  }

  void run_tick_thread() {
    // Transitional macOS publication loop: it consumes the shared presentation
    // scheduler, then asks Swift to copy the selected frame into the Flutter
    // texture. Renderer-owned Metal presentation should replace this path.
    constexpr auto kMaxPresentationSleep = std::chrono::microseconds(8000);
    std::unique_lock<std::mutex> lock(tick_mutex);
    while (!tick_stop.load()) {
      tick_cv.wait_for(lock, next_tick_sleep);
      if (tick_stop.load()) {
        break;
      }
      lock.unlock();
      vr::PresentationSchedulerTick tick;
      std::chrono::microseconds sleep_for = std::chrono::milliseconds(1);
      {
        std::lock_guard<std::mutex> player_lock(mutex);
        tick = core.tick_playback();
        sleep_for = core.next_presentation_sleep(kMaxPresentationSleep);
      }
      next_tick_sleep = sleep_for.count() > 0
          ? sleep_for
          : std::chrono::milliseconds(1);
      if (!tick.should_notify) {
        lock.lock();
        continue;
      }
      VPMacOSFrameAvailableCallback callback = nullptr;
      void* user_data = nullptr;
      bool renderer_owned_upload_succeeded = false;
      {
        std::lock_guard<std::mutex> callback_lock(callback_mutex);
        callback = frame_available_callback;
        user_data = frame_available_user_data;
        if (presentation_target_backend && presentation_target_pixel_buffer &&
            presentation_target_width > 0 && presentation_target_height > 0) {
          VPMacOSNativeFrameInfo frame_info = {};
          char error[256] = {};
          const int upload_ret = VPMacOSMetalPresentationBackendCopyCurrentFrameWithLayout(
              presentation_target_backend,
              this,
              presentation_target_pixel_buffer,
              presentation_target_width,
              presentation_target_height,
              presentation_target_max_track_slots,
              0,
              &frame_info,
              error,
              sizeof(error));
          renderer_owned_upload_succeeded = upload_ret == 0;
        }
        last_renderer_owned_presentation_succeeded = renderer_owned_upload_succeeded;
      }
      if (callback) {
        callback(user_data);
      }
      lock.lock();
    }
  }

  std::mutex mutex;
  MacOSNativePlayerCore core;
  std::mutex callback_mutex;
  VPMacOSFrameAvailableCallback frame_available_callback = nullptr;
  void* frame_available_user_data = nullptr;
  VPMacOSMetalPresentationBackend* presentation_target_backend = nullptr;
  void* presentation_target_pixel_buffer = nullptr;
  int32_t presentation_target_width = 0;
  int32_t presentation_target_height = 0;
  int32_t presentation_target_max_track_slots = 1;
  bool last_renderer_owned_presentation_succeeded = false;
  std::mutex tick_mutex;
  std::condition_variable tick_cv;
  std::chrono::microseconds next_tick_sleep{std::chrono::milliseconds(1)};
  std::atomic<bool> tick_stop{false};
  std::thread tick_thread;
};

VPMacOSNativePlayer* VPMacOSNativePlayerCreate(void) {
  auto* player = new (std::nothrow) VPMacOSNativePlayer();
  if (!player) {
    return nullptr;
  }
  if (!player->start_tick_thread()) {
    delete player;
    return nullptr;
  }
  return player;
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
  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (!player->core.open(path, message)) {
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
  if (!player->core.add_track(path, file_id, *out, message)) {
    write_error(error, error_size, message);
    return -1;
  }
  write_error(error, error_size, "");
  return 0;
}

void VPMacOSNativePlayerRemoveTrack(VPMacOSNativePlayer* player,
                                    int32_t file_id) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.remove_track(file_id);
}

void VPMacOSNativePlayerClose(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.close();
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
  if (!player || !backend || !pixel_buffer || width <= 0 || height <= 0) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  player->presentation_target_backend = backend;
  player->presentation_target_pixel_buffer = pixel_buffer;
  player->presentation_target_width = width;
  player->presentation_target_height = height;
  player->presentation_target_max_track_slots =
      std::clamp(max_track_slots, static_cast<int32_t>(1),
                 static_cast<int32_t>(VPMacOSNativeMaxTracks));
  player->last_renderer_owned_presentation_succeeded = false;
  return 0;
}

void VPMacOSNativePlayerClearMetalPresentationTarget(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  player->presentation_target_backend = nullptr;
  player->presentation_target_pixel_buffer = nullptr;
  player->presentation_target_width = 0;
  player->presentation_target_height = 0;
  player->presentation_target_max_track_slots = 1;
  player->last_renderer_owned_presentation_succeeded = false;
}

int VPMacOSNativePlayerRendererOwnedPresentationActive(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  return player->presentation_target_backend && player->presentation_target_pixel_buffer
      ? 1
      : 0;
}

int VPMacOSNativePlayerLastRendererOwnedPresentationSucceeded(
    VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  return player->last_renderer_owned_presentation_succeeded ? 1 : 0;
}

void VPMacOSNativePlayerPlay(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.play();
}

void VPMacOSNativePlayerPause(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.pause();
}

void VPMacOSNativePlayerSetSpeed(VPMacOSNativePlayer* player, double speed) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.set_speed(speed);
}

void VPMacOSNativePlayerSetLoopRange(VPMacOSNativePlayer* player,
                                     int enabled,
                                     int64_t start_us,
                                     int64_t end_us) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.set_loop_range(enabled != 0, start_us, end_us);
}

void VPMacOSNativePlayerSetAudibleTrack(VPMacOSNativePlayer* player,
                                        int32_t file_id) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.set_audible_track(file_id);
}

void VPMacOSNativePlayerSetTrackOffset(VPMacOSNativePlayer* player,
                                       int32_t file_id,
                                       int64_t offset_us) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.set_track_offset(file_id, offset_us);
}

int64_t VPMacOSNativePlayerTrackOffsetUs(VPMacOSNativePlayer* player,
                                         int32_t file_id) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.track_offset_us(file_id);
}

void VPMacOSNativePlayerApplyLayout(VPMacOSNativePlayer* player,
                                    const VPMacOSNativeLayoutState* state) {
  if (!player || !state) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.apply_layout(*state);
}

int VPMacOSNativePlayerCopyLayout(VPMacOSNativePlayer* player,
                                  VPMacOSNativeLayoutState* out) {
  if (!player || !out) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  *out = player->core.layout_snapshot();
  return 0;
}

int VPMacOSNativePlayerCopyLayoutPresentationParams(
    VPMacOSNativePlayer* player,
    int32_t width,
    int32_t height,
    VPMacOSNativeLayoutPresentationParams* out) {
  if (!player || !out) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.layout_presentation_params(width, height, out) ? 0 : -1;
}

void VPMacOSNativePlayerSeek(VPMacOSNativePlayer* player, int64_t pts_us) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.seek(pts_us);
}

int64_t VPMacOSNativePlayerCurrentPtsUs(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.current_pts_us();
}

int64_t VPMacOSNativePlayerDurationUs(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.duration_us();
}

int32_t VPMacOSNativePlayerWidth(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.width();
}

int32_t VPMacOSNativePlayerHeight(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.height();
}

int VPMacOSNativePlayerIsPlaying(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.is_playing() ? 1 : 0;
}

int VPMacOSNativePlayerHasAudio(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.has_audio() ? 1 : 0;
}

int32_t VPMacOSNativePlayerAudioSampleRate(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.audio_sample_rate();
}

int32_t VPMacOSNativePlayerAudioChannels(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.audio_channels();
}

int32_t VPMacOSNativePlayerActiveAudioTrack(VPMacOSNativePlayer* player) {
  if (!player) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.active_audio_track();
}

int VPMacOSNativePlayerHardwareDecodeActive(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.hardware_decode_active() ? 1 : 0;
}

int VPMacOSNativePlayerHardwareDecodeDownloadsToCpu(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.hardware_decode_downloads_to_cpu() ? 1 : 0;
}

const char* VPMacOSNativePlayerDecodeModeName(VPMacOSNativePlayer* player) {
  if (!player) {
    return "none";
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.decode_mode_name();
}

const char* VPMacOSNativePlayerDecoderName(VPMacOSNativePlayer* player) {
  if (!player) {
    return "none";
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.decoder_name();
}

const char* VPMacOSNativePresentationAdapterName(void) {
  return vp_macos::presentation_adapter_name();
}

const char* VPMacOSNativePresentationSchedulerName(void) {
  return "shared-presentation-scheduler/transitional-thread";
}

int VPMacOSNativePlayerCopyPresentationSchedulerStats(
    VPMacOSNativePlayer* player,
    VPMacOSNativePresentationSchedulerStats* out) {
  if (!player || !out) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  *out = player->core.scheduler_stats();
  return 0;
}

int VPMacOSNativeHardwareDecodeAvailable(void) {
  static const bool available = probe_videotoolbox_h264();
  return available ? 1 : 0;
}

const char* VPMacOSNativeHardwareDecodeProviderName(void) {
  return VPMacOSNativeHardwareDecodeAvailable() != 0 ? "VideoToolbox" : "none";
}

int VPMacOSNativePlayerCopyCurrentFrameBGRA(VPMacOSNativePlayer* player,
                                            VPMacOSNativeFrame* out,
                                            char* error,
                                            size_t error_size) {
  if (!player || !out) {
    write_error(error, error_size, "player or output frame is null");
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (!player->core.copy_current_frame(out, message)) {
    write_error(error, error_size, message);
    return -1;
  }
  write_error(error, error_size, "");
  return 0;
}

int VPMacOSNativePlayerCopyCurrentFrameBGRAInto(VPMacOSNativePlayer* player,
                                                uint8_t* dst,
                                                size_t dst_size,
                                                int32_t width,
                                                int32_t height,
                                                int32_t stride_bytes,
                                                VPMacOSNativeFrameInfo* out,
                                                char* error,
                                                size_t error_size) {
  if (!player || !dst || !out) {
    write_error(error, error_size, "player, destination, or output info is null");
    return -1;
  }
  *out = {};
  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (!player->core.copy_current_frame_into(
          dst, dst_size, width, height, stride_bytes, out, message)) {
    write_error(error, error_size, message);
    return -1;
  }
  write_error(error, error_size, "");
  return 0;
}

int VPMacOSNativePlayerCopyPresentFramesBGRAInto(
    VPMacOSNativePlayer* player,
    uint8_t* dst,
    size_t dst_size,
    int32_t width,
    int32_t height,
    int32_t stride_bytes,
    size_t track_stride_bytes,
    VPMacOSNativePresentDecisionInfo* out,
    char* error,
    size_t error_size) {
  if (!player || !dst || !out) {
    write_error(error, error_size, "player, destination, or present decision output is null");
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (!player->core.copy_present_frames_into(
          dst, dst_size, width, height, stride_bytes, track_stride_bytes, out, message)) {
    write_error(error, error_size, message);
    return -1;
  }
  write_error(error, error_size, "");
  return 0;
}

int VPMacOSNativePlayerCopyPresentFramesYUVInto(
    VPMacOSNativePlayer* player,
    uint8_t* dst,
    size_t dst_size,
    int32_t width,
    int32_t height,
    size_t max_track_slots,
    VPMacOSNativePresentDecisionInfo* out,
    char* error,
    size_t error_size) {
  if (!player || !dst || !out) {
    write_error(error, error_size, "player, destination, or present decision output is null");
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (!player->core.copy_present_frames_yuv_into(
          dst, dst_size, width, height, max_track_slots, out, message)) {
    write_error(error, error_size, message);
    return -1;
  }
  write_error(error, error_size, "");
  return 0;
}

size_t VPMacOSNativePresentFramePackageMaxBytes(int32_t width,
                                                int32_t height,
                                                int32_t max_track_slots) {
  const int32_t track_slots =
      std::clamp(max_track_slots, static_cast<int32_t>(1),
                 static_cast<int32_t>(VPMacOSNativeMaxTracks));
  return vr::describe_presentation_package_layout(width, height, track_slots).max_bytes;
}

int VPMacOSNativePlayerCopyPresentFramePackage(
    VPMacOSNativePlayer* player,
    uint8_t* dst,
    size_t dst_size,
    int32_t width,
    int32_t height,
    int32_t max_track_slots,
    VPMacOSNativePresentFramePackageInfo* out,
    char* error,
    size_t error_size) {
  if (!player || !dst || !out || width <= 0 || height <= 0) {
    write_error(error, error_size, "player, destination, or present package output is null");
    return -1;
  }
  *out = {};
  const int32_t track_slots =
      std::clamp(max_track_slots, static_cast<int32_t>(1),
                 static_cast<int32_t>(VPMacOSNativeMaxTracks));
  const auto package_layout =
      vr::describe_presentation_package_layout(width, height, track_slots);
  if (package_layout.bgra_max_bytes == 0 || package_layout.yuv_max_bytes == 0 ||
      package_layout.bgra_row_bytes >
          static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    write_error(error, error_size, "present frame package dimensions overflow");
    return -1;
  }

  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (dst_size >= package_layout.yuv_max_bytes &&
      player->core.copy_present_frames_yuv_into(
          dst,
          package_layout.yuv_max_bytes,
          width,
          height,
          static_cast<size_t>(track_slots),
          &out->decision,
          message)) {
    out->storage = VPMacOSNativePresentPackageStorageYUV;
    out->width = width;
    out->height = height;
    out->max_track_slots = track_slots;
    out->used_bytes = package_layout.yuv_max_bytes;
    write_error(error, error_size, "");
    return 0;
  }

  message.clear();
  out->decision = {};
  if (dst_size >= package_layout.bgra_max_bytes &&
      player->core.copy_present_frames_into(
          dst,
          package_layout.bgra_max_bytes,
          width,
          height,
          static_cast<int32_t>(package_layout.bgra_row_bytes),
          package_layout.bgra_track_stride_bytes,
          &out->decision,
          message)) {
    out->storage = VPMacOSNativePresentPackageStorageBGRA;
    out->width = width;
    out->height = height;
    out->max_track_slots = track_slots;
    out->stride_bytes = static_cast<int32_t>(package_layout.bgra_row_bytes);
    out->track_stride_bytes = package_layout.bgra_track_stride_bytes;
    out->used_bytes = package_layout.bgra_max_bytes;
    write_error(error, error_size, "");
    return 0;
  }

  if (message.empty()) {
    message = dst_size < package_layout.max_bytes
        ? "present frame package destination is too small"
        : "failed to copy native present frame package";
  }
  write_error(error, error_size, message);
  return -1;
}

int VPMacOSNativePlayerCopyRetainedCVPixelBufferPresentFrame(
    VPMacOSNativePlayer* player,
    int32_t width,
    int32_t height,
    VPMacOSNativeCVPixelBufferPresentFrame* out,
    char* error,
    size_t error_size) {
  if (!player || !out || width <= 0 || height <= 0) {
    write_error(error, error_size, "player or CVPixelBuffer present output is null");
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (!player->core.copy_retained_cv_pixel_buffer_present_frame(
          width, height, out, message)) {
    write_error(error, error_size, message);
    return -1;
  }
  write_error(error, error_size, "");
  return 0;
}

void VPMacOSNativeReleaseRetainedCVPixelBuffer(void* pixel_buffer) {
  if (pixel_buffer) {
    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(pixel_buffer));
  }
}

void VPMacOSNativeFrameFree(VPMacOSNativeFrame* frame) {
  vp_macos::free_owned_bgra_frame(frame);
}

int VPMacOSMeasureBGRA(const uint8_t* bgra,
                       int32_t width,
                       int32_t height,
                       int32_t stride_bytes,
                       VPMacOSCaptureMetrics* out) {
  if (!out) {
    return -1;
  }
  const auto metrics = vr::measure_bgra_capture(
      bgra, width, height, stride_bytes);
  out->width = metrics.width;
  out->height = metrics.height;
  out->avg_luma = metrics.avg_luma;
  out->non_black_ratio = metrics.non_black_ratio;
  out->hash = metrics.hash;
  return bgra ? 0 : -1;
}
