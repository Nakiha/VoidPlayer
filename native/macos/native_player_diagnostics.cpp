#include "macos/native_player_bridge.h"

#include "macos/native_player_state.h"
#include "macos/presentation_adapter.h"
#include "video_renderer/capture/bgra_capture_metrics.h"

#include <chrono>
#include <mutex>
#include <vector>

#include <mach/mach.h>

namespace {

void write_c_string(char* dest, size_t dest_size, const std::string& value) {
  vp_macos::write_error(dest, dest_size, value);
}

const vr::TrackPerfStats* find_perf_stats(
    const std::vector<vr::TrackPerfStats>& stats,
    int file_id) {
  for (const auto& perf : stats) {
    if (perf.file_id == file_id) {
      return &perf;
    }
  }
  return nullptr;
}

void fill_process_memory_stats(VPMacOSNativePlayerPerfStats* out) {
  if (!out) {
    return;
  }
  task_vm_info_data_t vm_info = {};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  const kern_return_t result = task_info(
      mach_task_self(),
      TASK_VM_INFO,
      reinterpret_cast<task_info_t>(&vm_info),
      &count);
  if (result == KERN_SUCCESS) {
    out->process_rss_bytes = static_cast<uint64_t>(vm_info.resident_size);
    out->process_private_bytes = static_cast<uint64_t>(vm_info.phys_footprint);
  }
}

}  // namespace

int VPMacOSNativePlayerRendererOwnedPresentationActive(VPMacOSNativePlayer* player) {
  VPMacOSNativeRendererOwnedPresentationState state = {};
  if (VPMacOSNativePlayerCopyRendererOwnedPresentationState(player, &state) != 0) {
    return 0;
  }
  return state.renderer_initialized != 0 &&
                 state.target_installed != 0 &&
                 state.backend_available != 0 &&
                 state.last_draw_succeeded != 0
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

int VPMacOSNativePlayerCopyRendererOwnedPresentationState(
    VPMacOSNativePlayer* player,
    VPMacOSNativeRendererOwnedPresentationState* out) {
  if (!player || !out) {
    return -1;
  }
  *out = {};
  std::string last_error;
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    out->target_installed =
        player->presentation_target_pixel_buffer &&
                player->presentation_target_width > 0 &&
                player->presentation_target_height > 0
            ? 1
            : 0;
    out->target_width = player->presentation_target_width;
    out->target_height = player->presentation_target_height;
    out->target_generation = player->presentation_target_generation;
    out->last_draw_succeeded =
        player->last_renderer_owned_presentation_succeeded ? 1 : 0;
    out->consecutive_draw_failures =
        player->renderer_owned_presentation_consecutive_failures;
    out->draw_failure_count =
        player->renderer_owned_presentation_draw_failure_count;
    out->upload_count = player->renderer_owned_presentation_upload_count;
    out->upload_failure_count = player->renderer_owned_presentation_failure_count;
    out->last_successful_frame_pts_us =
        player->last_renderer_owned_frame_info_available
            ? player->last_renderer_owned_frame_info.pts_us
            : 0;
    last_error = player->renderer_owned_presentation_last_error;
  }
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    out->renderer_initialized = player->renderer_active_locked() ? 1 : 0;
    if (player->renderer_active_locked()) {
      const auto backend_stats = player->renderer->presentation_backend_stats();
      out->backend_available = backend_stats.backend_available;
      out->target_installed =
          out->target_installed != 0 && backend_stats.target_installed != 0 ? 1 : 0;
      out->last_draw_succeeded = backend_stats.last_draw_succeeded;
      out->upload_storage_kind = backend_stats.last_present_package_storage;
      out->overlay_last_expected = backend_stats.overlay_last_expected;
      out->overlay_last_applied = backend_stats.overlay_last_applied;
      out->overlay_last_line_rect_count = backend_stats.overlay_last_line_rect_count;
      out->overlay_expected_count = backend_stats.overlay_expected_count;
      out->overlay_applied_count = backend_stats.overlay_applied_count;
      out->overlay_missed_count = backend_stats.overlay_missed_count;
      out->overlay_gpu_success_count = backend_stats.overlay_gpu_success_count;
      out->overlay_gpu_failure_count = backend_stats.overlay_gpu_failure_count;
      out->overlay_cpu_fallback_count = backend_stats.overlay_cpu_fallback_count;
      if (backend_stats.last_successful_frame_pts_us != 0) {
        out->last_successful_frame_pts_us =
            backend_stats.last_successful_frame_pts_us;
      }
    }
  }
  vp_macos::write_error(
      out->last_draw_error, sizeof(out->last_draw_error), last_error);
  return 0;
}

int VPMacOSNativePlayerCopyTrackDiagnostics(
    VPMacOSNativePlayer* player,
    VPMacOSNativeTrackDiagnosticInfo* out,
    size_t capacity,
    size_t* out_count) {
  if (out_count) {
    *out_count = 0;
  }
  if (!player) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (!player->renderer_active_locked()) {
    return 0;
  }
  const auto infos = player->renderer->track_infos();
  const auto perf_stats = player->renderer->track_perf_stats();
  const auto memory_stats = player->renderer->gpu_memory_stats();
  const size_t count = infos.size();
  if (out_count) {
    *out_count = count;
  }
  if (!out || capacity == 0) {
    return 0;
  }
  const size_t copy_count = std::min(capacity, count);
  for (size_t i = 0; i < copy_count; ++i) {
    const auto& info = infos[i];
    auto& dst = out[i];
    dst = {};
    dst.file_id = info.file_id;
    dst.slot = info.slot;
    dst.width = info.width;
    dst.height = info.height;
    dst.duration_us = info.duration_us;
    dst.offset_us = player->renderer->track_offset_us(info.file_id);
    const std::string decoder_name =
        info.decoder_name.empty() ? "renderer" : info.decoder_name;
    const bool videotoolbox =
        vp_macos::decoder_name_is_videotoolbox(decoder_name);
    dst.hardware_decode_active = videotoolbox ? 1 : 0;
    dst.hardware_decode_downloads_to_cpu =
        videotoolbox && vp_macos::videotoolbox_hwdownload_forced_by_env() ? 1 : 0;
    const auto* perf = find_perf_stats(perf_stats, info.file_id);
    if (perf) {
      dst.buffer_state = static_cast<int32_t>(perf->buffer_state);
      dst.buffer_count = perf->buffer_count;
      dst.buffer_capacity = perf->buffer_capacity;
      dst.frames_decoded = perf->frames_decoded;
      dst.decode_fps = perf->fps;
      dst.decode_avg_ms = perf->avg_decode_ms;
      dst.decode_max_ms = perf->max_decode_ms;
      dst.current_pts_us = perf->current_pts_us;
      dst.current_dts_us = perf->current_dts_us;
    }
    for (const auto& memory : memory_stats.tracks) {
      if (memory.slot == info.slot && memory.file_id == info.file_id) {
        dst.cpu_frame_memory_bytes = memory.total_cpu_frame_bytes;
        dst.packet_queue_memory_bytes = memory.packet_queue_bytes;
        break;
      }
    }
    write_c_string(dst.codec_name, sizeof(dst.codec_name), info.codec_name);
    write_c_string(dst.decoder_name, sizeof(dst.decoder_name), decoder_name);
    write_c_string(dst.decode_mode,
                   sizeof(dst.decode_mode),
                   videotoolbox ? "shared-renderer-videotoolbox"
                                : "shared-renderer-software");
  }
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
  player->renderer_owned_presentation_draw_failure_count = 0;
  player->renderer_owned_presentation_consecutive_failures = 0;
  player->renderer_owned_presentation_last_error.clear();
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

int VPMacOSNativePlayerHardwareDecodeActive(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->update_decode_names_locked();
  return player->decode_mode_name == "shared-renderer-videotoolbox" ? 1 : 0;
}

int VPMacOSNativePlayerHardwareDecodeDownloadsToCpu(VPMacOSNativePlayer* player) {
  return player && vp_macos::videotoolbox_hwdownload_forced_by_env() ? 1 : 0;
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
  fill_process_memory_stats(out);
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
      out->active_track_count = stats.size();
      for (const auto& track_stats : stats) {
        out->aggregate_decode_frame_count += track_stats.frames_decoded;
        out->aggregate_decode_fps += track_stats.fps;
      }
      const auto memory_stats = player->renderer->gpu_memory_stats();
      out->cpu_frame_memory_bytes = memory_stats.cpu_frame_bytes;
      out->packet_queue_memory_bytes = memory_stats.packet_queue_bytes;
      const auto backend_stats = player->renderer->presentation_backend_stats();
      const auto backend_metrics = player->renderer->presentation_backend_metrics();
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
      out->renderer_owned_staging_allocation_count =
          backend_stats.staging_allocation_count;
      out->renderer_owned_staging_reuse_count =
          backend_stats.staging_reuse_count;
      out->renderer_owned_staging_max_bytes =
          backend_stats.staging_max_bytes;
      out->renderer_draw_count = backend_metrics.draw_count;
      if (backend_metrics.draw_count > 0) {
        out->renderer_draw_avg_us =
            static_cast<int64_t>(backend_metrics.draw_total_us /
                                 backend_metrics.draw_count);
        out->renderer_draw_backend_avg_us =
            static_cast<int64_t>(backend_metrics.draw_backend_total_us /
                                 backend_metrics.draw_count);
      }
      out->renderer_draw_max_us =
          static_cast<int64_t>(backend_metrics.draw_max_us);
      out->renderer_draw_p95_us =
          static_cast<int64_t>(backend_metrics.draw_p95_us);
      out->renderer_draw_backend_max_us =
          static_cast<int64_t>(backend_metrics.draw_backend_max_us);
      out->renderer_draw_backend_p95_us =
          static_cast<int64_t>(backend_metrics.draw_backend_p95_us);
      out->renderer_layout_intent_count = backend_metrics.layout_intent_count;
      out->renderer_layout_presented_count = backend_metrics.layout_presented_count;
      out->renderer_layout_deferred_to_playback_count =
          backend_metrics.layout_deferred_to_playback_count;
      out->renderer_playing_layout_redraw_suppressed_count =
          backend_metrics.playing_layout_redraw_suppressed_count;
      out->renderer_layout_stale_completion_drop_count =
          backend_metrics.layout_stale_completion_drop_count;
      out->renderer_last_layout_revision = backend_metrics.last_layout_revision;
      out->renderer_last_presented_layout_revision =
          backend_metrics.last_presented_layout_revision;
      if (backend_metrics.layout_presented_count > 0) {
        out->renderer_draws_per_presented_layout_x1000 =
            static_cast<int64_t>(
                backend_metrics.draw_count * 1000 /
                backend_metrics.layout_presented_count);
      }
      out->in_flight_metal_buffer_count =
          backend_stats.in_flight_metal_buffer_count;
      out->metal_buffer_exhaustion_count =
          backend_stats.metal_buffer_exhaustion_count;
      out->metal_command_completion_p95_us =
          backend_stats.metal_command_completion_p95_us;
      out->metal_command_failure_count =
          backend_stats.metal_command_failure_count;
      out->async_metal_publish_active =
          backend_stats.async_metal_publish_active;
      out->video_source_update_count =
          backend_stats.video_source_update_count;
      out->viewport_composite_count =
          backend_stats.viewport_composite_count;
      out->source_frame_cache_hit_count =
          backend_stats.source_frame_cache_hit_count;
      out->source_frame_cache_miss_count =
          backend_stats.source_frame_cache_miss_count;
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

int VPMacOSNativePlayerCopyAudioDiagnostics(
    VPMacOSNativePlayer* player,
    VPMacOSNativeAudioDiagnostics* out) {
  if (!player || !out) {
    return -1;
  }
  *out = {};
  std::lock_guard<std::mutex> lock(player->mutex);
  if (!player->renderer_active_locked()) {
    return 0;
  }
  const auto stats = player->renderer->audio_output_stats();
  out->device_initialized = stats.device_initialized ? 1 : 0;
  out->playing = stats.playing ? 1 : 0;
  out->active_track = stats.active_track;
  out->output_sample_rate = stats.output_sample_rate;
  out->output_channels = stats.output_channels;
  out->registered_track_count = stats.registered_track_count;
  out->active_track_registered = stats.active_track_registered ? 1 : 0;
  out->active_track_queued_frames = stats.active_track_queued_frames;
  out->active_track_queued_duration_us = stats.active_track_queued_duration_us;
  out->active_track_underrun_frames = stats.active_track_underrun_frames;
  out->active_track_discarded_frames = stats.active_track_discarded_frames;
  out->active_track_seek_trimmed_frames = stats.active_track_seek_trimmed_frames;
  return 0;
}

int VPMacOSNativeHardwareDecodeAvailable(void) {
  static const bool available = vp_macos::probe_videotoolbox_h264();
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
