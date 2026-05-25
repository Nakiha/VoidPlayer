#include "macos/native_player_bridge.h"

#include "macos/native_player_state.h"
#include "macos/presentation_adapter.h"
#include "video_renderer/capture/bgra_capture_metrics.h"

#include <chrono>
#include <mutex>

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
