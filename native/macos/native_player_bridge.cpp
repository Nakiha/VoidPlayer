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
#include "video_renderer/render/shader_constants.h"
#include "video_renderer/capture/bgra_capture_metrics.h"
#include "video_renderer/seek/seek_coordinator.h"
#include "video_renderer/sync/render_sink.h"

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

bool videotoolbox_disabled_by_env() {
  const char* value = std::getenv("VOIDPLAYER_DISABLE_VIDEOTOOLBOX");
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
  params.device_mode = vr::DecodeDeviceMode::FfmpegOwnedHwDownloadDevice;
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

struct MacOSNativeTrackRuntime {
  int32_t file_id = -1;
  int32_t slot = -1;
  uint64_t generation = 1;
  std::string path;
  std::unique_ptr<vr::SeekController> seek_controller;
  std::unique_ptr<vr::PacketQueue> packet_queue;
  std::shared_ptr<vr::TrackBuffer> track_buffer;
  std::unique_ptr<vr::DemuxThread> demux;
  std::unique_ptr<vr::DecodeThread> decoder;
  int32_t width = 0;
  int32_t height = 0;
  int64_t duration_us = 0;
  int64_t offset_us = 0;
};

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
    audio_packet_queue_ = std::make_unique<vr::PacketQueue>(96);

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
    last_tick_frame_pts_us_ = std::numeric_limits<int64_t>::min();
    return true;
  }

  void close() {
    playing_ = false;
    for (auto& track : tracks_) {
      stop_track(track);
      track.reset();
    }
    playback_.stop_session();
    render_sink_.reset();
    audio_packet_queue_.reset();
    width_ = 0;
    height_ = 0;
    duration_us_ = 0;
    audio_available_ = false;
    audio_sample_rate_ = 0;
    audio_channels_ = 0;
    loop_range_ = vr::LoopRangeState();
    layout_controller_.reset(layout_);
    last_tick_frame_pts_us_ = std::numeric_limits<int64_t>::min();
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
      const float aspect = track->height > 0
          ? static_cast<float>(track->width) / static_cast<float>(track->height)
          : 1.0f;
      tracks[static_cast<size_t>(track->slot)] = {
          true,
          track->width,
          track->height,
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
    const size_t min_track_stride =
        static_cast<size_t>(stride_bytes) * static_cast<size_t>(height);
    if (track_stride_bytes < min_track_stride ||
        track_stride_bytes > std::numeric_limits<size_t>::max() / vr::kMaxTracks ||
        dst_size < track_stride_bytes * vr::kMaxTracks) {
      error = "present decision BGRA destination is too small";
      return false;
    }
    if (!render_sink_) {
      error = "player is not open";
      return false;
    }

    const auto decision = render_sink_->evaluate();
    fill_present_decision_info(decision, width, height, out);
    if (!decision.should_present) {
      error = "no presentable frame is ready";
      return false;
    }
    if (out->track_count > 1 && out->frame_count < out->track_count) {
      error = "not all present decision frames are ready";
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
    if (audio_packet_queue_) {
      audio_packet_queue_->flush();
    }
    playback_.seek_clock(target);
    last_tick_frame_pts_us_ = std::numeric_limits<int64_t>::min();
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
    return primary && primary->decoder &&
        primary->decoder->is_hardware_decode_enabled();
  }
  bool hardware_decode_downloads_to_cpu() const {
    const auto* primary = find_track_by_file_id(0);
    return primary && primary->decoder &&
        primary->decoder->memory_stats().hardware_download_to_cpu;
  }
  const char* decode_mode_name() const {
    const auto* primary = find_track_by_file_id(0);
    if (!primary || !primary->decoder) {
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
    if (!primary || !primary->decoder) {
      return "none";
    }
    return hardware_decode_active()
        ? "decode_thread_videotoolbox_hwdownload"
        : "decode_thread_software";
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

  bool tick_playback() {
    tick_loop_range();
    if (!playing_) {
      return false;
    }
    int64_t frame_pts_us = std::numeric_limits<int64_t>::min();
    if (!advance_to_clock(&frame_pts_us)) {
      return false;
    }
    if (frame_pts_us == last_tick_frame_pts_us_) {
      return false;
    }
    last_tick_frame_pts_us_ = frame_pts_us;
    return true;
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
  bool advance_to_clock(int64_t* frame_pts_us) {
    if (!render_sink_) {
      return false;
    }
    const auto decision = render_sink_->evaluate();
    const auto& frame = decision.frames[0];
    if (!decision.should_present || !frame.has_value()) {
      return false;
    }
    if (frame_pts_us) {
      *frame_pts_us = frame->pts_us;
    }
    return true;
  }

  vr::LayoutTrackGeometryList layout_track_geometry() const {
    vr::LayoutTrackGeometryList tracks = {};
    for (const auto& track : tracks_) {
      if (!track || track->slot < 0 || track->slot >= static_cast<int32_t>(tracks.size())) {
        continue;
      }
      const float aspect = track->height > 0
          ? static_cast<float>(track->width) / static_cast<float>(track->height)
          : 1.0f;
      tracks[static_cast<size_t>(track->slot)] = {
          true,
          track->width,
          track->height,
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
    out->should_present = decision.should_present ? 1 : 0;
    out->current_pts_us = decision.current_pts_us;
    const auto tracks = layout_track_geometry();
    vr::ShaderConstants constants = {};
    vr::populate_layout_shader_constants(constants, layout_, tracks, width, height);
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
      frame_out.file_id = decision.file_ids[slot];
      frame_out.slot = static_cast<int32_t>(slot);
      if (!decision.frames[slot].has_value()) {
        continue;
      }
      const auto& frame = *decision.frames[slot];
      frame_out.present = 1;
      frame_out.width = frame.width;
      frame_out.height = frame.height;
      out->source_width[slot] = frame.width;
      out->source_height[slot] = frame.height;
      frame_out.pts_us = frame.pts_us;
      frame_out.dts_us = frame.dts_us;
      frame_out.duration_us = frame.duration_us;
      ++out->frame_count;
    }
  }

  std::unique_ptr<vr::PacketQueue> audio_packet_queue_;
  std::unique_ptr<vr::RenderSink> render_sink_;
  std::array<std::unique_ptr<MacOSNativeTrackRuntime>, vr::kMaxTracks> tracks_{};
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
  int64_t last_tick_frame_pts_us_ = std::numeric_limits<int64_t>::min();
  bool playing_ = false;

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

  MacOSNativeTrackRuntime* find_track_by_file_id(int32_t file_id) {
    const int slot = slot_for_file_id(file_id);
    return slot >= 0 ? tracks_[static_cast<size_t>(slot)].get() : nullptr;
  }

  const MacOSNativeTrackRuntime* find_track_by_file_id(int32_t file_id) const {
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

    auto track = std::make_unique<MacOSNativeTrackRuntime>();
    track->file_id = file_id;
    track->slot = slot;
    track->generation = 1;
    track->path = path;
    track->seek_controller = std::make_unique<vr::SeekController>();
    track->packet_queue = std::make_unique<vr::PacketQueue>(96);
    track->track_buffer = std::make_shared<vr::TrackBuffer>(16, 4);
    track->demux = std::make_unique<vr::DemuxThread>(
        track->path, *track->packet_queue, *track->seek_controller);
    if (primary && audio_packet_queue_) {
      track->demux->add_optional_output(vr::DemuxStreamKind::Audio, *audio_packet_queue_);
    }

    if (!track->demux->open()) {
      error = "failed to open demux input";
      return false;
    }
    const auto& stats = track->demux->stats();
    if (!stats.codec_params || stats.video_stream_index < 0 || stats.time_base.den == 0) {
      error = "input has no usable video stream";
      return false;
    }

    track->width = stats.width;
    track->height = stats.height;
    track->duration_us = stats.duration_us;

    if (primary) {
      duration_us_ = stats.duration_us;
      audio_sample_rate_ = stats.sample_rate;
      audio_channels_ = stats.channels;
      playback_.stop_session();
      if (stats.audio_stream_index >= 0 && stats.audio_codec_params) {
        playback_.start_session();
        auto* audio = playback_.audio_output();
        audio_available_ = audio &&
            audio->add_track(0, *audio_packet_queue_, stats.audio_codec_params,
                             stats.audio_time_base);
        if (audio_available_) {
          audio->set_active_track(0);
          audio->set_all_decode_paused(true);
        } else {
          playback_.stop_session();
        }
      }
    }

    track->decoder = std::make_unique<vr::DecodeThread>(
        *track->packet_queue, *track->track_buffer, stats.codec_params, stats.time_base);
    if (!track->decoder->is_valid()) {
      error = "decode thread failed to initialize";
      return false;
    }
    if (!videotoolbox_disabled_by_env()) {
      track->decoder->enable_hardware_decode(
          vr::DecodeDeviceMode::FfmpegOwnedHwDownloadDevice);
    }
    auto* decoder = track->decoder.get();
    auto* audio_output = primary ? playback_.audio_output() : nullptr;
    track->demux->set_seek_callback(
        [decoder, audio_output, primary](int64_t pts_us, vr::SeekType type) {
          if (decoder) {
            decoder->notify_seek(pts_us, type);
          }
          if (primary && audio_output) {
            audio_output->notify_seek(0, pts_us, type);
          }
        });
    if (!track->decoder->start()) {
      error = "decode thread failed to start";
      return false;
    }
    if (!track->demux->start_thread()) {
      error = "demux thread failed to start";
      track->decoder->stop();
      return false;
    }

    if (render_sink_) {
      render_sink_->set_track(
          static_cast<size_t>(slot), track->track_buffer, file_id, track->generation);
    }
    layout_controller_.append_track(layout_, file_id, slot);
    out.file_id = file_id;
    out.slot = slot;
    out.width = track->width;
    out.height = track->height;
    out.duration_us = track->duration_us;
    tracks_[static_cast<size_t>(slot)] = std::move(track);
    recompute_duration();
    return true;
  }

  void stop_track(std::unique_ptr<MacOSNativeTrackRuntime>& track) {
    if (!track) {
      return;
    }
    if (track->decoder) {
      track->decoder->stop();
    }
    if (track->demux) {
      track->demux->stop();
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
    std::unique_lock<std::mutex> lock(tick_mutex);
    while (!tick_stop.load()) {
      tick_cv.wait_for(lock, std::chrono::milliseconds(10));
      if (tick_stop.load()) {
        break;
      }
      lock.unlock();
      bool notify_frame = false;
      {
        std::lock_guard<std::mutex> player_lock(mutex);
        notify_frame = core.tick_playback();
      }
      if (!notify_frame) {
        lock.lock();
        continue;
      }
      VPMacOSFrameAvailableCallback callback = nullptr;
      void* user_data = nullptr;
      {
        std::lock_guard<std::mutex> callback_lock(callback_mutex);
        callback = frame_available_callback;
        user_data = frame_available_user_data;
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
  std::mutex tick_mutex;
  std::condition_variable tick_cv;
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
