#include "native_player_bridge.h"

#include "macos/metal_presentation_backend.h"
#include "macos/presentation_adapter.h"
#include "macos/presentation_package_builder.h"
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
#include "video_renderer/render/presentation_loop_driver.h"
#include "video_renderer/render/presentation_snapshot.h"
#include "video_renderer/render/shader_constants.h"
#include "video_renderer/capture/bgra_capture_metrics.h"
#include "video_renderer/playback/renderer_playback_command_policy.h"
#include "video_renderer/seek/seek_coordinator.h"
#include "video_renderer/sync/render_sink.h"
#include "video_renderer/track/track_preview_policy.h"
#include "video_renderer/track/track_pipeline_factory.h"
#include "video_renderer/track/track_present_policy.h"
#include "video_renderer/track/track_step_policy.h"

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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

bool is_transient_presentation_error(const std::string& error) {
  return error == "no presentable frame is ready" ||
         error == "not all present decision frames are ready";
}

void write_error(char* error, size_t error_size, const std::string& message) {
  if (!error || error_size == 0) {
    return;
  }
  const size_t copy_size = std::min(error_size - 1, message.size());
  std::memcpy(error, message.data(), copy_size);
  error[copy_size] = '\0';
}

void clear_bgra_canvas(uint8_t* dst,
                       size_t dst_size,
                       int32_t width,
                       int32_t height,
                       int32_t stride_bytes) {
  if (!dst || width <= 0 || height <= 0 || stride_bytes < width * 4) {
    return;
  }
  const size_t needed = static_cast<size_t>(height - 1) *
      static_cast<size_t>(stride_bytes) + static_cast<size_t>(width) * 4u;
  if (needed > dst_size) {
    return;
  }
  for (int32_t y = 0; y < height; ++y) {
    auto* row = dst + static_cast<size_t>(y) * static_cast<size_t>(stride_bytes);
    std::memset(row, 0, static_cast<size_t>(width) * 4u);
    for (int32_t x = 0; x < width; ++x) {
      row[static_cast<size_t>(x) * 4u + 3u] = 255;
    }
  }
}

int ordered_track_at(const VPMacOSNativePresentDecisionInfo& info, size_t index) {
  if (index >= static_cast<size_t>(VPMacOSNativeMaxTracks)) {
    return 0;
  }
  return std::clamp(info.order[index], 0, VPMacOSNativeMaxTracks - 1);
}

bool bgra_canvas_source_uv(const VPMacOSNativePresentDecisionInfo& info,
                           int32_t width,
                           int32_t height,
                           int32_t x,
                           int32_t y,
                           int* out_track,
                           float* out_u,
                           float* out_v) {
  if (!out_track || !out_u || !out_v || width <= 0 || height <= 0) {
    return false;
  }
  const float tex_u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
  const float tex_v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
  int track = 0;
  float local_u = tex_u;
  const float local_v = tex_v;
  if (info.mode == vr::LAYOUT_SPLIT_SCREEN) {
    track = tex_u < info.split_pos ? ordered_track_at(info, 0) : ordered_track_at(info, 1);
  } else {
    const int count = std::max(info.track_count, 1);
    const float scaled_x = tex_u * static_cast<float>(count);
    const int display_slot = std::clamp(static_cast<int>(scaled_x), 0, count - 1);
    track = ordered_track_at(info, static_cast<size_t>(display_slot));
    local_u = scaled_x - static_cast<float>(display_slot);
  }
  track = std::clamp(track, 0, VPMacOSNativeMaxTracks - 1);
  const float source_u =
      (local_u - info.display_offset_x[track]) * info.inv_display_size_x[track] -
      info.view_offset_uv_x[track];
  const float source_v =
      (local_v - info.display_offset_y[track]) * info.inv_display_size_y[track] -
      info.view_offset_uv_y[track];
  if (!std::isfinite(source_u) || !std::isfinite(source_v) ||
      source_u < 0.0f || source_u > 1.0f || source_v < 0.0f || source_v > 1.0f) {
    return false;
  }
  *out_track = track;
  *out_u = source_u;
  *out_v = source_v;
  return true;
}

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
    reset_scheduler_stats();
    perf_start_time_ = std::chrono::steady_clock::now();
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
    reset_scheduler_stats();
    perf_start_time_ = std::chrono::steady_clock::now();
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

  vr::RendererDrawSnapshot draw_snapshot_for_decision(
      const vr::PresentDecision& decision,
      int32_t width,
      int32_t height) const {
    vr::RendererDrawSnapshot snapshot;
    snapshot.decision = decision;
    vr::filter_present_decision_against_tracks(snapshot.decision, tracks_);
    snapshot.layout = layout_;
    snapshot.track_geometry = layout_track_geometry();
    for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
      const auto& track = tracks_[slot];
      if (!track) {
        continue;
      }
      auto& out = snapshot.tracks[slot];
      out.active = true;
      out.file_id = track->file_id;
      out.generation = track->generation;
      out.offset_us = track->offset_us;
      out.video_width = track->video_width;
      out.video_height = track->video_height;
      out.video_aspect = track->video_aspect;
    }
    snapshot.target_width = width;
    snapshot.target_height = height;
    return snapshot;
  }

  vr::RendererDrawSnapshot draw_snapshot_for_current_frame(int32_t width,
                                                           int32_t height) {
    auto decision = current_present_decision();
    if (!decision.should_present) {
      decision = primary_peek_present_decision();
    }
    return draw_snapshot_for_decision(decision, width, height);
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

    const auto snapshot =
        draw_snapshot_for_decision(current_present_decision(), width, height);
    VPMacOSNativePresentFramePackageInfo package = {};
    const bool copied = vp_macos::copy_snapshot_bgra_package(
        snapshot,
        dst,
        dst_size,
        width,
        height,
        stride_bytes,
        track_stride_bytes,
        &package,
        error);
    *out = package.decision;
    return copied;
  }

  bool copy_present_canvas_bgra_into(uint8_t* dst,
                                     size_t dst_size,
                                     int32_t width,
                                     int32_t height,
                                     int32_t stride_bytes,
                                     VPMacOSNativeFrameInfo* out,
                                     std::string& error) {
    if (!dst || !out || width <= 0 || height <= 0 || stride_bytes < width * 4) {
      error = "invalid present canvas BGRA destination";
      return false;
    }
    const size_t last_row_offset =
        static_cast<size_t>(height - 1) * static_cast<size_t>(stride_bytes);
    const size_t needed = last_row_offset + static_cast<size_t>(width) * 4u;
    if (needed > dst_size) {
      error = "present canvas BGRA destination is too small";
      return false;
    }
    if (!render_sink_) {
      error = "player is not open";
      return false;
    }

    const auto decision = current_present_decision();
    VPMacOSNativePresentDecisionInfo info = {};
    fill_present_decision_info(decision, width, height, &info);
    if (!decision.should_present) {
      error = "no presentable frame is ready";
      return false;
    }
    if (info.track_count > 1 && info.frame_count < info.track_count) {
      error = "not all present decision frames are ready";
      return false;
    }

    std::array<VPMacOSNativeFrame, vr::kMaxTracks> owned_frames{};
    auto release_owned = [&]() {
      for (auto& frame : owned_frames) {
        vp_macos::free_owned_bgra_frame(&frame);
      }
    };

    for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
      if (!decision.frames[slot].has_value()) {
        continue;
      }
      if (!vp_macos::copy_texture_frame_to_owned_bgra(*decision.frames[slot],
                                                      &owned_frames[slot])) {
        release_owned();
        error = "decoded frame storage is not supported by the macOS BGRA layout adapter";
        return false;
      }
    }

    clear_bgra_canvas(dst, dst_size, width, height, stride_bytes);
    for (int32_t y = 0; y < height; ++y) {
      auto* dst_row = dst + static_cast<size_t>(y) * static_cast<size_t>(stride_bytes);
      for (int32_t x = 0; x < width; ++x) {
        int track = 0;
        float source_u = 0.0f;
        float source_v = 0.0f;
        if (!bgra_canvas_source_uv(info, width, height, x, y,
                                   &track, &source_u, &source_v)) {
          continue;
        }
        const auto& frame = owned_frames[static_cast<size_t>(track)];
        if (!frame.bgra || frame.width <= 0 || frame.height <= 0) {
          continue;
        }
        const int32_t source_x = std::clamp(
            static_cast<int32_t>(source_u * static_cast<float>(frame.width)),
            0,
            frame.width - 1);
        const int32_t source_y = std::clamp(
            static_cast<int32_t>(source_v * static_cast<float>(frame.height)),
            0,
            frame.height - 1);
        const size_t source_offset =
            (static_cast<size_t>(source_y) * static_cast<size_t>(frame.width) +
             static_cast<size_t>(source_x)) *
            4u;
        const size_t dst_offset = static_cast<size_t>(x) * 4u;
        if (source_offset + 4u <= frame.bgra_size) {
          std::memcpy(dst_row + dst_offset, frame.bgra + source_offset, 4u);
        }
      }
    }

    for (const auto& frame : owned_frames) {
      if (frame.bgra) {
        out->width = frame.width;
        out->height = frame.height;
        out->pts_us = frame.pts_us;
        out->dts_us = frame.dts_us;
        out->duration_us = frame.duration_us;
        break;
      }
    }
    release_owned();
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

    const auto snapshot =
        draw_snapshot_for_decision(current_present_decision(), width, height);
    VPMacOSNativePresentFramePackageInfo package = {};
    const bool copied = vp_macos::copy_snapshot_yuv_package(
        snapshot,
        dst,
        dst_size,
        width,
        height,
        max_track_slots,
        &package,
        error);
    *out = package.decision;
    return copied;
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
    auto decision = current_present_decision();
    if (!decision.should_present) {
      decision = primary_peek_present_decision();
    }
    const auto snapshot = draw_snapshot_for_decision(decision, width, height);
    return vp_macos::snapshot_cv_pixel_buffer_frame(
        snapshot, width, height, true, out, error);
  }

  void seek(int64_t pts_us, vr::SeekType type = vr::SeekType::Exact) {
    const int64_t target = std::max<int64_t>(0, pts_us);
    for (auto& track : tracks_) {
      if (!track || !track->seek_controller || !track->track_buffer || !track->packet_queue) {
        continue;
      }
      const int64_t track_target = std::max<int64_t>(0, target - track->offset_us);
      track->track_buffer->set_state(vr::TrackState::Flushing);
      track->track_buffer->clear_frames();
      track->packet_queue->flush();
      track->seek_controller->request_seek(track_target, type);
    }
    auto* primary = find_track_by_file_id(0);
    if (primary && primary->audio_packet_queue) {
      primary->audio_packet_queue->flush();
    }
    playback_.seek_clock(target);
    presentation_loop_driver_.reset_presentation_state();
  }

  bool step_forward(std::string& error) {
    if (!render_sink_ || !tracks_.has_active_tracks()) {
      error = "player is not open";
      return false;
    }
    const auto step_plan =
        vr::plan_renderer_step_command(true, vr::has_buffering_track(tracks_));
    if (!step_plan.execute) {
      error = "step forward is not ready";
      return false;
    }

    if (step_plan.pause_clock) {
      pause();
    }
    const auto set_video_decode_paused = [this](bool paused) {
      vr::apply_track_video_decode_pause_state(
          tracks_,
          paused,
          [](size_t, vr::TrackPipeline& track, bool next_paused) {
            if (track.decode_thread) {
              track.decode_thread->set_decode_paused(next_paused);
            }
          });
    };
    const auto build_decision = [this](const vr::PresentDecision& last_decision,
                                       vr::PresentDecision& decision) {
      return vr::build_step_forward_decision(
          tracks_,
          playback_.clock().current_pts_us(),
          vr::compute_min_current_frame_duration_us(tracks_),
          last_decision,
          decision);
    };
    const auto apply_decision = [this](const vr::PresentDecision& last_decision,
                                       const vr::PresentDecision& decision) {
      return vr::apply_step_forward_decision(
          tracks_,
          playback_.clock().current_pts_us(),
          decision,
          last_decision);
    };

    vr::PresentDecision last_decision = current_present_decision();
    vr::PresentDecision step_decision;
    if (build_decision(last_decision, step_decision)) {
      const auto application = apply_decision(last_decision, step_decision);
      if (application.has_clock_target) {
        playback_.clock().seek(application.clock_target_us);
        step_decision.current_pts_us = application.clock_target_us;
      }
      presentation_loop_driver_.publish_present_decision(step_decision);
      return true;
    }

    vr::discard_step_forward_consumed_frames(
        tracks_, playback_.clock().current_pts_us(), last_decision, last_decision);
    set_video_decode_paused(false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(180);
    while (std::chrono::steady_clock::now() < deadline) {
      if (build_decision(last_decision, step_decision)) {
        set_video_decode_paused(true);
        const auto application = apply_decision(last_decision, step_decision);
        if (application.has_clock_target) {
          playback_.clock().seek(application.clock_target_us);
          step_decision.current_pts_us = application.clock_target_us;
        }
        presentation_loop_driver_.publish_present_decision(step_decision);
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    set_video_decode_paused(true);
    const auto fallback_seek = vr::choose_step_forward_exact_seek_target(
        tracks_,
        playback_.clock().current_pts_us(),
        duration_us_,
        last_decision);
    seek(fallback_seek.target_pts_us, vr::SeekType::ExactStepForward);
    return true;
  }

  bool step_backward(std::string& error) {
    if (!render_sink_ || !tracks_.has_active_tracks()) {
      error = "player is not open";
      return false;
    }
    const auto step_plan =
        vr::plan_renderer_step_command(true, vr::has_buffering_track(tracks_));
    if (!step_plan.execute) {
      error = "step backward is not ready";
      return false;
    }

    if (step_plan.pause_clock) {
      pause();
    }
    if (vr::retreat_tracks_if_all_can_retreat(tracks_)) {
      const auto application = vr::choose_step_backward_retreat_application(tracks_);
      if (application.has_clock_target) {
        playback_.clock().seek(application.clock_target_us);
      }
      auto snapshot = vr::build_available_paused_frame_snapshot(tracks_);
      if (snapshot.has_frame) {
        snapshot.decision.current_pts_us = playback_.clock().current_pts_us();
        snapshot.decision.should_present = true;
        presentation_loop_driver_.publish_present_decision(snapshot.decision);
      }
      return true;
    }

    const auto fallback_seek = vr::choose_step_backward_exact_seek_target(
        tracks_, playback_.clock().current_pts_us());
    seek(fallback_seek.target_pts_us, vr::SeekType::Exact);
    return true;
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

  vr::DecodePerfCounters::Snapshot primary_decode_perf_snapshot() const {
    const auto* primary = find_track_by_file_id(0);
    if (!primary || !primary->decode_thread) {
      return {};
    }
    return primary->decode_thread->perf_counters().snapshot();
  }

  int64_t perf_elapsed_ms() const {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now - perf_start_time_)
        .count();
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

  vr::PresentationLoopDriverTick tick_playback(std::chrono::microseconds max_sleep) {
    tick_loop_range();
    if (!render_sink_) {
      return {};
    }
    const int64_t current_pts_us = playback_.clock().current_pts_us();
    const auto previous_decision = current_present_decision();
    auto tick = presentation_loop_driver_.tick(
        *render_sink_,
        playing_,
        current_pts_us,
        playback_.clock().speed(),
        vr::compute_next_frame_event_pts_us(tracks_, current_pts_us),
        max_sleep);
    if (tick.scheduler.has_presentable_frame) {
      auto decision = tick.scheduler.decision;
      filter_present_decision_against_tracks(decision, tracks_);
      apply_present_carry_forward(tracks_, previous_decision, decision);
      presentation_loop_driver_.publish_present_decision(decision);
      tick.scheduler.decision = decision;
    }
    settle_eof_if_ready();
    return tick;
  }

  VPMacOSNativePresentationSchedulerStats scheduler_stats() const {
    VPMacOSNativePresentationSchedulerStats stats = {};
    const auto driver_stats = presentation_loop_driver_.stats();
    stats.tick_count = driver_stats.tick_count;
    stats.presentable_tick_count = driver_stats.presentable_tick_count;
    stats.frame_notification_count = driver_stats.frame_notification_count;
    stats.last_selected_pts_us = driver_stats.last_selected_pts_us;
    stats.last_present_frame_count = driver_stats.last_present_frame_count;
    stats.cached_present_decision_available =
        driver_stats.cached_present_decision_available ? 1 : 0;
    stats.deadline_sleep_count = driver_stats.deadline_sleep_count;
    stats.last_deadline_sleep_us = driver_stats.last_deadline_sleep_us;
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
  vr::PresentDecision current_present_decision() {
    return presentation_loop_driver_.current_present_decision(render_sink_.get());
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
    presentation_loop_driver_.clear_cached_present_decision();
  }

  bool advance_to_clock(int64_t* frame_pts_us) {
    if (!render_sink_) {
      return false;
    }
    return presentation_loop_driver_.advance_to_clock(*render_sink_, frame_pts_us);
  }

  void set_decode_paused_for_all_tracks(bool paused) {
    vr::apply_track_video_decode_pause_state(
        tracks_,
        paused,
        [](size_t, vr::TrackPipeline& track, bool next_paused) {
          if (track.decode_thread) {
            track.decode_thread->set_decode_paused(next_paused);
          }
        });
    if (auto* audio = playback_.audio_output()) {
      audio->set_all_decode_paused(paused);
    }
  }

  bool settle_eof_if_ready() {
    if (!playing_) {
      return false;
    }
    auto decision = current_present_decision();
    vr::filter_present_decision_against_tracks(decision, tracks_);
    const auto eof_clamp = vr::compute_empty_buffer_eof_clamp(tracks_, decision);
    if (!eof_clamp.all_active_buffers_empty || eof_clamp.max_end_pts_us <= 0) {
      return false;
    }
    for (const auto& track : tracks_) {
      if (!track || !track->packet_queue) {
        continue;
      }
      if (!track->packet_queue->empty() || !track->packet_queue->is_eof()) {
        return false;
      }
    }

    const auto settlement = vr::choose_playback_eof_settlement({
        playing_,
        playback_.clock().current_pts_us(),
        eof_clamp.max_end_pts_us,
        duration_us_,
        vr::compute_min_current_frame_duration_us(tracks_),
    });
    if (!settlement.should_settle) {
      return false;
    }

    set_decode_paused_for_all_tracks(true);
    playback_.clock().seek(settlement.settle_pts_us);
    playback_.clock().pause();
    playing_ = false;
    return true;
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
  vr::TrackPipelineManager tracks_;
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
  vr::PresentationLoopDriver presentation_loop_driver_;
  bool playing_ = false;
  std::chrono::steady_clock::time_point perf_start_time_ =
      std::chrono::steady_clock::now();

  void reset_scheduler_stats() {
    presentation_loop_driver_.reset();
  }

  int first_free_slot() const {
    return tracks_.find_empty_slot();
  }

  int slot_for_file_id(int file_id) const {
    return tracks_.find_slot_by_file_id(file_id);
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
    if (slot < 0 || slot >= static_cast<int>(vr::kMaxTracks) ||
        tracks_[static_cast<size_t>(slot)]) {
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
      vr::PresentationLoopDriverTick tick;
      std::chrono::microseconds sleep_for = std::chrono::milliseconds(1);
      {
        std::lock_guard<std::mutex> player_lock(mutex);
        tick = core.tick_playback(kMaxPresentationSleep);
        sleep_for = tick.next_sleep;
      }
      next_tick_sleep = sleep_for.count() > 0
          ? sleep_for
          : std::chrono::milliseconds(1);
      if (!tick.scheduler.should_notify) {
        lock.lock();
        continue;
      }
      VPMacOSFrameAvailableCallback callback = nullptr;
      void* user_data = nullptr;
      bool renderer_owned_upload_attempted = false;
      bool renderer_owned_upload_succeeded = false;
      bool renderer_owned_upload_pending = false;
      VPMacOSNativeFrameInfo renderer_owned_frame_info = {};
      {
        std::lock_guard<std::mutex> callback_lock(callback_mutex);
        callback = frame_available_callback;
        user_data = frame_available_user_data;
        if (presentation_target_backend && presentation_target_pixel_buffer &&
            presentation_target_width > 0 && presentation_target_height > 0) {
          renderer_owned_upload_attempted = true;
          vr::RendererDrawSnapshot draw_snapshot;
          {
            std::lock_guard<std::mutex> player_lock(mutex);
            draw_snapshot = core.draw_snapshot_for_decision(
                tick.scheduler.decision,
                presentation_target_width,
                presentation_target_height);
          }
          vr::PresentationBackendDrawHooks draw_hooks;
          renderer_owned_upload_succeeded =
              presentation_target_backend->impl.draw_frame(draw_snapshot, draw_hooks);
          renderer_owned_upload_pending =
              is_transient_presentation_error(
                  presentation_target_backend->impl.last_error());
          if (renderer_owned_upload_succeeded) {
            presentation_target_backend->impl.copy_last_draw_frame_info(
                &renderer_owned_frame_info);
          }
        }
        last_renderer_owned_presentation_succeeded = renderer_owned_upload_succeeded;
        last_renderer_owned_frame_info_available = renderer_owned_upload_succeeded;
        if (renderer_owned_upload_succeeded) {
          last_renderer_owned_frame_info = renderer_owned_frame_info;
        }
        if (renderer_owned_upload_attempted) {
          if (renderer_owned_upload_succeeded) {
            const auto now = std::chrono::steady_clock::now();
            if (renderer_owned_presentation_upload_count == 0) {
              renderer_owned_presentation_first_upload_time = now;
            }
            renderer_owned_presentation_last_upload_time = now;
            ++renderer_owned_presentation_upload_count;
          } else if (!renderer_owned_upload_pending) {
            ++renderer_owned_presentation_failure_count;
          }
        }
      }
      if (renderer_owned_upload_attempted && !renderer_owned_upload_succeeded) {
        lock.lock();
        continue;
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
  bool last_renderer_owned_frame_info_available = false;
  VPMacOSNativeFrameInfo last_renderer_owned_frame_info = {};
  uint64_t renderer_owned_presentation_upload_count = 0;
  uint64_t renderer_owned_presentation_failure_count = 0;
  std::chrono::steady_clock::time_point renderer_owned_presentation_first_upload_time{};
  std::chrono::steady_clock::time_point renderer_owned_presentation_last_upload_time{};
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
  VPMacOSMetalPresentationBackendSetDrawTarget(
      backend,
      pixel_buffer,
      width,
      height,
      player->presentation_target_max_track_slots);
  player->last_renderer_owned_presentation_succeeded = false;
  player->last_renderer_owned_frame_info_available = false;
  player->last_renderer_owned_frame_info = {};
  return 0;
}

void VPMacOSNativePlayerClearMetalPresentationTarget(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  auto* backend = player->presentation_target_backend;
  player->presentation_target_backend = nullptr;
  player->presentation_target_pixel_buffer = nullptr;
  player->presentation_target_width = 0;
  player->presentation_target_height = 0;
  player->presentation_target_max_track_slots = 1;
  if (backend) {
    VPMacOSMetalPresentationBackendClearDrawTarget(backend);
  }
  player->last_renderer_owned_presentation_succeeded = false;
  player->last_renderer_owned_frame_info_available = false;
  player->last_renderer_owned_frame_info = {};
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
  std::lock_guard<std::mutex> callback_lock(player->callback_mutex);
  if (!player->presentation_target_backend ||
      !player->presentation_target_pixel_buffer ||
      player->presentation_target_width <= 0 ||
      player->presentation_target_height <= 0) {
    write_error(error, error_size, "renderer-owned presentation target is not installed");
    return -1;
  }

  vr::RendererDrawSnapshot draw_snapshot;
  {
    std::lock_guard<std::mutex> player_lock(player->mutex);
    draw_snapshot = player->core.draw_snapshot_for_current_frame(
        player->presentation_target_width,
        player->presentation_target_height);
  }
  if (!draw_snapshot.decision.should_present) {
    player->last_renderer_owned_presentation_succeeded = false;
    player->last_renderer_owned_frame_info_available = false;
    write_error(error, error_size, "no presentable frame is ready");
    return -1;
  }

  vr::PresentationBackendDrawHooks draw_hooks;
  const bool succeeded =
      player->presentation_target_backend->impl.draw_frame(draw_snapshot, draw_hooks);
  player->last_renderer_owned_presentation_succeeded = succeeded;
  player->last_renderer_owned_frame_info_available = succeeded;
  if (succeeded &&
      player->presentation_target_backend->impl.copy_last_draw_frame_info(out)) {
    player->last_renderer_owned_frame_info = *out;
    const auto now = std::chrono::steady_clock::now();
    if (player->renderer_owned_presentation_upload_count == 0) {
      player->renderer_owned_presentation_first_upload_time = now;
    }
    player->renderer_owned_presentation_last_upload_time = now;
    ++player->renderer_owned_presentation_upload_count;
    write_error(error, error_size, "");
    return 0;
  }

  player->last_renderer_owned_presentation_succeeded = false;
  player->last_renderer_owned_frame_info_available = false;
  const auto& backend_error =
      player->presentation_target_backend->impl.last_error();
  if (!is_transient_presentation_error(backend_error)) {
    ++player->renderer_owned_presentation_failure_count;
  }
  write_error(error,
              error_size,
              backend_error.empty()
                  ? "renderer-owned presentation upload failed"
                  : backend_error.c_str());
  return -1;
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

int VPMacOSNativePlayerStepForward(VPMacOSNativePlayer* player,
                                   char* error,
                                   size_t error_size) {
  if (!player) {
    write_error(error, error_size, "player is null");
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (!player->core.step_forward(message)) {
    write_error(error, error_size, message);
    return -1;
  }
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
  std::string message;
  if (!player->core.step_backward(message)) {
    write_error(error, error_size, message);
    return -1;
  }
  write_error(error, error_size, "");
  return 0;
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

int VPMacOSNativePlayerCopyPerfStats(
    VPMacOSNativePlayer* player,
    VPMacOSNativePlayerPerfStats* out) {
  if (!player || !out) {
    return -1;
  }
  *out = {};
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    const auto decode = player->core.primary_decode_perf_snapshot();
    out->decode_frame_count = decode.frames_decoded;
    out->decode_dropped_count = decode.frames_dropped;
    out->decode_elapsed_ms = player->core.perf_elapsed_ms();
    if (decode.frames_decoded > 0) {
      out->decode_avg_ms =
          static_cast<double>(decode.total_decode_us) /
          static_cast<double>(decode.frames_decoded) / 1000.0;
    }
    out->decode_max_ms = static_cast<double>(decode.max_decode_us) / 1000.0;
    if (out->decode_elapsed_ms > 0) {
      out->decode_fps =
          static_cast<double>(decode.frames_decoded) * 1000.0 /
          static_cast<double>(out->decode_elapsed_ms);
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

int VPMacOSNativePlayerCopyPresentationBGRAInto(
    VPMacOSNativePlayer* player,
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
  if (!player->core.copy_present_canvas_bgra_into(
          dst, dst_size, width, height, stride_bytes, out, message)) {
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
