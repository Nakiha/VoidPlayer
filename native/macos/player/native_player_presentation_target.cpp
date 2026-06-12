#include "native_player_bridge.h"

#include "macos/metal/metal_presentation_backend.h"
#include "macos/player/native_player_state.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

using vp_macos::write_error;

namespace {

bool macos_profiler_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("VOIDPLAYER_MACOS_PROFILER");
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0;
  }();
  return enabled;
}

uint64_t pointer_address(const void* pointer) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer));
}

std::string target_address_summary(const std::vector<void*>& targets) {
  std::ostringstream stream;
  for (void* target : targets) {
    stream << "0x" << std::hex << pointer_address(target) << std::dec << ",";
  }
  return stream.str();
}

void copy_frame_info(const vr::PresentationBackendFrameInfo& source,
                     VPMacOSNativeFrameInfo* out) {
  if (!out) {
    return;
  }
  VPMacOSNativeFrameInfoInit(out);
  out->width = source.width;
  out->height = source.height;
  out->pts_us = source.pts_us;
  out->dts_us = source.dts_us;
  out->duration_us = source.duration_us;
  out->analysis_frame_index = source.analysis_frame_index;
  out->frame_identity_mode = source.frame_identity_mode;
  out->source_packet_index = source.source_packet_index;
  out->source_packet_size = source.source_packet_size;
  out->source_packet_pos = source.source_packet_pos;
  out->source_packet_pts = source.source_packet_pts;
  out->source_packet_dts = source.source_packet_dts;
  out->color_range = source.color_range;
  out->color_matrix = source.color_matrix;
  out->color_transfer = source.color_transfer;
  out->color_primaries = source.color_primaries;
  out->target_pixel_buffer_address = source.target_pixel_buffer_address;
  out->layout_revision = source.layout_revision;
}

}  // namespace

void VPMacOSNativePlayerSetFrameAvailableCallback(
    VPMacOSNativePlayer* player,
    VPMacOSFrameAvailableCallback callback,
    void* user_data) {
  if (!player) {
    return;
  }
  std::unique_lock<std::mutex> lock(player->callback_mutex);
  ++player->frame_available_callback_generation;
  player->frame_available_callback = callback;
  player->frame_available_user_data = user_data;
  if (!callback) {
    player->callback_condition.wait(lock, [player] {
      return player->frame_available_callback_in_flight == 0;
    });
  }
}

namespace {

int set_metal_presentation_target(
    VPMacOSNativePlayer* player,
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots,
    bool refresh_now) {
  if (!player || !pixel_buffer || width <= 0 || height <= 0) {
    return -1;
  }
  const int32_t clamped_track_slots =
      std::clamp(max_track_slots, static_cast<int32_t>(1),
                 static_cast<int32_t>(VPMacOSNativeMaxTracks));
  bool target_changed = false;
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    target_changed =
        player->presentation_target_backend != backend ||
        player->presentation_target_pixel_buffer != pixel_buffer ||
        player->presentation_target_width != width ||
        player->presentation_target_height != height ||
        player->presentation_target_max_track_slots != clamped_track_slots;
    player->presentation_target_backend = backend;
    player->presentation_target_pixel_buffer = pixel_buffer;
    player->presentation_target_pixel_buffers.clear();
    player->presentation_target_pixel_buffers.push_back(pixel_buffer);
    player->presentation_target_width = width;
    player->presentation_target_height = height;
    player->presentation_target_max_track_slots = clamped_track_slots;
    if (target_changed) {
      ++player->presentation_target_generation;
      spdlog::info(
          "[MacOSFrameRefresh] install_target generation={} target=0x{:x} "
          "size={}x{} slots={} refresh={}",
          player->presentation_target_generation,
          pointer_address(pixel_buffer),
          width,
          height,
          clamped_track_slots,
          refresh_now);
      if (refresh_now) {
        player->last_renderer_owned_presentation_succeeded = false;
        player->last_renderer_owned_frame_info_available = false;
        player->last_renderer_owned_frame_info = {};
        player->renderer_owned_presentation_consecutive_failures = 0;
        player->renderer_owned_presentation_last_error.clear();
      }
    }
  }
  if (target_changed) {
    player->presentation_condition.notify_all();
  }

  std::string renderer_error;
  {
    std::lock_guard<std::mutex> player_lock(player->mutex);
    if (player->renderer_active_locked()) {
      if (!target_changed) {
        return 0;
      }
      if (!refresh_now) {
        if (player->renderer->install_headless_output(
                pixel_buffer, width, height, clamped_track_slots)) {
          return 0;
        }
        renderer_error = "failed to install renderer-owned Metal presentation target";
      } else {
        if (player->renderer->update_headless_output(
                pixel_buffer, width, height, clamped_track_slots)) {
          return 0;
        }
      }
      if (renderer_error.empty()) {
        renderer_error = "failed to install renderer-owned Metal presentation target";
      }
    } else if (!player->opened_path.empty()) {
      if (player->ensure_renderer_locked(renderer_error)) {
        return 0;
      }
    } else {
      return 0;
    }
  }
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    player->record_presentation_failure_locked(renderer_error, true);
  }
  player->presentation_condition.notify_all();
  return -1;
}

}  // namespace

int VPMacOSNativePlayerSetMetalPresentationTarget(
    VPMacOSNativePlayer* player,
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots) {
  return set_metal_presentation_target(
      player, backend, pixel_buffer, width, height, max_track_slots, true);
}

int VPMacOSNativePlayerInstallMetalPresentationTarget(
    VPMacOSNativePlayer* player,
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots) {
  return set_metal_presentation_target(
      player, backend, pixel_buffer, width, height, max_track_slots, false);
}

int VPMacOSNativePlayerInstallMetalPresentationTargetRing(
    VPMacOSNativePlayer* player,
    VPMacOSMetalPresentationBackend* backend,
    const void* const* pixel_buffers,
    size_t pixel_buffer_count,
    void* displayed_pixel_buffer,
    void* protected_pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots) {
  if (!player || !backend || !pixel_buffers || pixel_buffer_count == 0 ||
      width <= 0 || height <= 0) {
    return -1;
  }
  const int32_t clamped_track_slots =
      std::clamp(max_track_slots, static_cast<int32_t>(1),
                 static_cast<int32_t>(VPMacOSNativeMaxTracks));
  std::vector<void*> targets;
  targets.reserve(pixel_buffer_count);
  for (size_t i = 0; i < pixel_buffer_count; ++i) {
    void* target = const_cast<void*>(pixel_buffers[i]);
    if (target) {
      targets.push_back(target);
    }
  }
  if (targets.empty()) {
    return -1;
  }
  bool target_changed = false;
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    target_changed =
        player->presentation_target_backend != backend ||
        player->presentation_target_pixel_buffers != targets ||
        player->presentation_target_width != width ||
        player->presentation_target_height != height ||
        player->presentation_target_max_track_slots != clamped_track_slots;
    player->presentation_target_backend = backend;
    player->presentation_target_pixel_buffer = targets.front();
    player->presentation_target_pixel_buffers = targets;
    player->presentation_target_width = width;
    player->presentation_target_height = height;
    player->presentation_target_max_track_slots = clamped_track_slots;
    if (target_changed) {
      ++player->presentation_target_generation;
      spdlog::info(
          "[MacOSFrameRefresh] install_target_ring generation={} targets=[{}] "
          "displayed=0x{:x} protected=0x{:x} size={}x{} slots={}",
          player->presentation_target_generation,
          target_address_summary(targets),
          pointer_address(displayed_pixel_buffer),
          pointer_address(protected_pixel_buffer),
          width,
          height,
          clamped_track_slots);
      player->last_renderer_owned_presentation_succeeded = false;
      player->last_renderer_owned_frame_info_available = false;
      player->last_renderer_owned_frame_info = {};
      player->renderer_owned_presentation_consecutive_failures = 0;
      player->renderer_owned_presentation_last_error.clear();
    }
  }
  if (target_changed) {
    player->presentation_condition.notify_all();
  }

  std::string renderer_error;
  {
    std::lock_guard<std::mutex> player_lock(player->mutex);
    if (player->renderer_active_locked()) {
      if (player->renderer->install_headless_output_ring(pixel_buffers,
                                                         pixel_buffer_count,
                                                         displayed_pixel_buffer,
                                                         protected_pixel_buffer,
                                                         width,
                                                         height,
                                                         clamped_track_slots)) {
        return 0;
      }
      renderer_error = "failed to install renderer-owned Metal presentation target ring";
    } else if (!player->opened_path.empty()) {
      if (!player->ensure_renderer_locked(renderer_error)) {
        std::lock_guard<std::mutex> lock(player->callback_mutex);
        player->record_presentation_failure_locked(renderer_error, true);
        player->presentation_condition.notify_all();
        return -1;
      }
      if (!player->renderer->install_headless_output_ring(pixel_buffers,
                                                          pixel_buffer_count,
                                                          displayed_pixel_buffer,
                                                          protected_pixel_buffer,
                                                          width,
                                                          height,
                                                          clamped_track_slots)) {
        std::lock_guard<std::mutex> lock(player->callback_mutex);
        player->record_presentation_failure_locked(
            "failed to install renderer-owned Metal presentation target ring", true);
        player->presentation_condition.notify_all();
        return -1;
      }
    }
  }
  if (!renderer_error.empty()) {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    player->record_presentation_failure_locked(renderer_error, true);
    player->presentation_condition.notify_all();
    return -1;
  }
  return 0;
}

void VPMacOSNativePlayerMarkMetalPresentationTargetDisplayed(
    VPMacOSNativePlayer* player,
    void* pixel_buffer) {
  if (!player || !pixel_buffer) {
    return;
  }
  VPMacOSMetalPresentationBackend* backend = nullptr;
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    backend = player->presentation_target_backend;
  }
  {
    std::lock_guard<std::mutex> player_lock(player->mutex);
    if (player->renderer_active_locked()) {
      player->renderer->mark_headless_output_displayed(pixel_buffer);
      return;
    }
  }
  VPMacOSMetalPresentationBackendMarkDisplayedTarget(backend, pixel_buffer);
}

void VPMacOSNativePlayerProtectMetalPresentationTarget(
    VPMacOSNativePlayer* player,
    void* pixel_buffer) {
  if (!player) {
    return;
  }
  VPMacOSMetalPresentationBackend* backend = nullptr;
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    backend = player->presentation_target_backend;
  }
  {
    std::lock_guard<std::mutex> player_lock(player->mutex);
    if (player->renderer_active_locked()) {
      player->renderer->protect_headless_output(pixel_buffer);
      return;
    }
  }
  VPMacOSMetalPresentationBackendProtectTarget(backend, pixel_buffer);
}

void VPMacOSNativePlayerReleaseMetalPresentationTarget(
    VPMacOSNativePlayer* player,
    void* pixel_buffer) {
  if (!player || !pixel_buffer) {
    return;
  }
  VPMacOSMetalPresentationBackend* backend = nullptr;
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    backend = player->presentation_target_backend;
  }
  {
    std::lock_guard<std::mutex> player_lock(player->mutex);
    if (player->renderer_active_locked()) {
      player->renderer->release_headless_output(pixel_buffer);
      return;
    }
  }
  VPMacOSMetalPresentationBackendReleaseTarget(backend, pixel_buffer);
}

void VPMacOSNativePlayerClearMetalPresentationTarget(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    player->presentation_target_backend = nullptr;
    player->presentation_target_pixel_buffer = nullptr;
    player->presentation_target_pixel_buffers.clear();
    player->presentation_target_width = 0;
    player->presentation_target_height = 0;
    player->presentation_target_max_track_slots = 1;
    ++player->presentation_target_generation;
    spdlog::info(
        "[MacOSFrameRefresh] clear_target generation={} upload={} failures={}",
        player->presentation_target_generation,
        player->renderer_owned_presentation_upload_count,
        player->renderer_owned_presentation_draw_failure_count);
    player->record_presentation_failure_locked(
        "renderer-owned Metal presentation target was cleared", false);
  }
  player->presentation_condition.notify_all();
  std::lock_guard<std::mutex> player_lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->clear_headless_output();
  }
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
  VPMacOSNativeFrameInfoInit(out);
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

namespace {

int request_renderer_owned_frame_refresh(
    VPMacOSNativePlayer* player,
    int32_t timeout_ms,
    uint32_t flags,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  const auto profiler_start = std::chrono::steady_clock::now();
  if (!player || !out) {
    write_error(error, error_size, "player or renderer-owned frame output is null");
    return -1;
  }
  VPMacOSNativeFrameInfoInit(out);
  const int32_t bounded_timeout_ms = std::max<int32_t>(0, timeout_ms);

  uint64_t baseline_upload_count = 0;
  uint64_t baseline_draw_failure_count = 0;
  uint64_t baseline_target_generation = 0;
  uint64_t baseline_target_address = 0;
  std::vector<uint64_t> baseline_target_addresses;
  bool baseline_frame_available = false;
  int64_t refresh_clock_us = 0;
  int64_t refresh_min_pts_us = -1;
  uint64_t expected_layout_revision = 0;
  int refresh_attempts = 0;
  bool refresh_submitted = false;
  bool refresh_deferred_by_backpressure = false;
  std::string last_refresh_backpressure_error;
  std::string last_refresh_renderer_error;
  const bool suppress_frame_callback =
      (flags & VPMacOSNativeFrameRefreshSuppressFrameCallback) != 0;
  auto release_manual_refresh_callback_suppression_locked = [&]() {
    if (!suppress_frame_callback) {
      return;
    }
    if (player->manual_refresh_callback_suppression_count > 0) {
      --player->manual_refresh_callback_suppression_count;
    }
  };
  auto release_manual_refresh_callback_suppression = [&]() {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    release_manual_refresh_callback_suppression_locked();
  };
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    if (!player->presentation_target_pixel_buffer ||
        player->presentation_target_width <= 0 ||
        player->presentation_target_height <= 0) {
      write_error(error, error_size,
                  "renderer-owned Metal presentation target is not installed");
      return -1;
    }
    baseline_upload_count = player->renderer_owned_presentation_upload_count;
    baseline_draw_failure_count =
        player->renderer_owned_presentation_draw_failure_count;
    baseline_target_generation = player->presentation_target_generation;
    baseline_target_address = pointer_address(player->presentation_target_pixel_buffer);
    baseline_target_addresses.clear();
    baseline_target_addresses.reserve(player->presentation_target_pixel_buffers.size());
    for (void* target : player->presentation_target_pixel_buffers) {
      baseline_target_addresses.push_back(pointer_address(target));
    }
    baseline_frame_available = player->last_renderer_owned_frame_info_available;
    refresh_min_pts_us = player->renderer_owned_refresh_min_pts_us;
    if (suppress_frame_callback) {
      ++player->manual_refresh_callback_suppression_count;
    }
  }
  const bool log_manual_refresh = refresh_min_pts_us >= 0;
  if (log_manual_refresh) {
    spdlog::info(
        "[MacOSFrameRefresh] begin min_pts_us={} timeout_ms={} baseline_upload={} "
        "target_generation={} suppress_callback={}",
        refresh_min_pts_us,
        bounded_timeout_ms,
        baseline_upload_count,
        baseline_target_generation,
        suppress_frame_callback);
  }

  auto trigger_renderer_refresh = [&]() -> bool {
    std::string message;
    std::lock_guard<std::mutex> lock(player->mutex);
    if (!player->ensure_renderer_locked(message)) {
      write_error(error, error_size, message);
      return false;
    }
    if (player->renderer) {
      refresh_clock_us = player->renderer->current_pts_us();
      ++refresh_attempts;
      refresh_submitted = false;
      refresh_deferred_by_backpressure = false;
      const char* refresh_reason =
          refresh_min_pts_us >= 0 ? "seek_frame_refresh"
                                  : "macos-renderer-owned-refresh";
      refresh_submitted =
          player->renderer->request_frame_refresh(refresh_reason);
      if (!refresh_submitted) {
        const auto renderer_error =
            player->renderer->presentation_backend_last_error();
        if (vr::is_transient_presentation_backpressure_error(renderer_error)) {
          refresh_deferred_by_backpressure = true;
          last_refresh_backpressure_error = renderer_error;
          write_error(error, error_size, renderer_error);
        } else if (!renderer_error.empty()) {
          last_refresh_renderer_error = renderer_error;
        }
      }
      if (refresh_submitted) {
        const auto metrics = player->renderer->presentation_backend_metrics();
        expected_layout_revision =
            std::max(expected_layout_revision, metrics.last_layout_revision);
      }
      return true;
    }
    write_error(error, error_size, "shared macOS renderer is not available");
    return false;
  };
  if (!trigger_renderer_refresh()) {
    release_manual_refresh_callback_suppression();
    return -1;
  }

  std::unique_lock<std::mutex> callback_lock(player->callback_mutex);
  const bool enforce_refresh_pts_window =
      refresh_min_pts_us >= 0 || (!baseline_frame_available && refresh_clock_us > 0);
  const auto frame_matches_refresh_request = [&]() {
    if (!player->last_renderer_owned_frame_info_available) {
      return false;
    }
    constexpr int64_t kRefreshPtsLowerToleranceUs = 500'000;
    constexpr int64_t kRefreshPtsUpperToleranceUs = 1'500'000;
    if (refresh_min_pts_us >= 0) {
      const int64_t pts_us = player->last_renderer_owned_frame_info.pts_us;
      return pts_us >= refresh_min_pts_us &&
             pts_us <= refresh_min_pts_us + kRefreshPtsLowerToleranceUs +
                           kRefreshPtsUpperToleranceUs;
    }
    if (!enforce_refresh_pts_window) {
      return true;
    }
    const int64_t pts_us = player->last_renderer_owned_frame_info.pts_us;
    return pts_us >= refresh_clock_us - kRefreshPtsLowerToleranceUs &&
           pts_us <= refresh_clock_us + kRefreshPtsUpperToleranceUs;
  };
  const auto frame_matches_layout_request = [&]() {
    return expected_layout_revision == 0 ||
           player->last_renderer_owned_layout_revision >=
               expected_layout_revision;
  };
  const auto frame_matches_target_request = [&]() {
    if (!player->last_renderer_owned_frame_info_available ||
        (baseline_target_address == 0 && baseline_target_addresses.empty())) {
      return false;
    }
    const uint64_t frame_target =
        player->last_renderer_owned_frame_info.target_pixel_buffer_address;
    if (!baseline_target_addresses.empty()) {
      return std::find(baseline_target_addresses.begin(),
                       baseline_target_addresses.end(),
                       frame_target) != baseline_target_addresses.end();
    }
    return frame_target == baseline_target_address;
  };
  const auto completed = [&]() {
    return player->presentation_target_generation != baseline_target_generation ||
           (player->renderer_owned_presentation_upload_count >
                baseline_upload_count &&
            frame_matches_target_request() &&
            frame_matches_layout_request() &&
            frame_matches_refresh_request()) ||
           player->renderer_owned_presentation_draw_failure_count >
               baseline_draw_failure_count;
  };
  if (bounded_timeout_ms > 0) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(bounded_timeout_ms);
    while (!completed()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - now);
      const auto wait_slice =
          std::min<std::chrono::milliseconds>(remaining,
                                              std::chrono::milliseconds(20));
      player->presentation_condition.wait_for(callback_lock, wait_slice, completed);
      if (completed()) {
        break;
      }
      if (!refresh_submitted) {
        callback_lock.unlock();
        const bool requested = trigger_renderer_refresh();
        callback_lock.lock();
        if (!requested) {
          release_manual_refresh_callback_suppression_locked();
          return -1;
        }
      }
    }
  }

  if (player->presentation_target_generation != baseline_target_generation) {
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - profiler_start).count();
    if (macos_profiler_enabled()) {
      spdlog::info(
          "[MacOSProfiler] request_frame_refresh changed_target elapsed_ms={} timeout_ms={} attempts={}",
          elapsed_ms,
          bounded_timeout_ms,
          refresh_attempts);
    }
    write_error(error, error_size,
                "renderer-owned Metal presentation target changed during refresh");
    release_manual_refresh_callback_suppression_locked();
    return -1;
  }
  if (player->renderer_owned_presentation_upload_count > baseline_upload_count &&
      frame_matches_target_request() &&
      frame_matches_layout_request() &&
      frame_matches_refresh_request()) {
    *out = player->last_renderer_owned_frame_info;
    if (refresh_min_pts_us >= 0) {
      player->renderer_owned_refresh_min_pts_us = -1;
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - profiler_start).count();
    if (macos_profiler_enabled() && (elapsed_ms >= 12 || refresh_attempts > 1)) {
      spdlog::info(
          "[MacOSProfiler] request_frame_refresh ok elapsed_ms={} timeout_ms={} attempts={} "
          "baseline_upload={} upload={} pts_us={} clock_us={}",
          elapsed_ms,
          bounded_timeout_ms,
          refresh_attempts,
          baseline_upload_count,
          player->renderer_owned_presentation_upload_count,
          out->pts_us,
          refresh_clock_us);
    }
    if (log_manual_refresh) {
      spdlog::info(
          "[MacOSFrameRefresh] presented pts_us={} dts_us={} duration_us={} "
          "clock_us={} min_pts_us={} elapsed_ms={} attempts={} upload={}->{}",
          out->pts_us,
          out->dts_us,
          out->duration_us,
          refresh_clock_us,
          refresh_min_pts_us,
          elapsed_ms,
          refresh_attempts,
          baseline_upload_count,
          player->renderer_owned_presentation_upload_count);
    }
    write_error(error, error_size, "");
    return 0;
  }
  if (player->renderer_owned_presentation_draw_failure_count >
      baseline_draw_failure_count) {
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - profiler_start).count();
    if (macos_profiler_enabled()) {
      spdlog::info(
          "[MacOSProfiler] request_frame_refresh draw_failed elapsed_ms={} timeout_ms={} attempts={} failures={} error={}",
          elapsed_ms,
          bounded_timeout_ms,
          refresh_attempts,
          player->renderer_owned_presentation_draw_failure_count,
          player->renderer_owned_presentation_last_error);
    }
    spdlog::warn(
        "[MacOSFrameRefresh] draw_failed elapsed_ms={} timeout_ms={} attempts={} "
        "min_pts_us={} baseline_upload={} upload={} error={}",
        elapsed_ms,
        bounded_timeout_ms,
        refresh_attempts,
        refresh_min_pts_us,
        baseline_upload_count,
        player->renderer_owned_presentation_upload_count,
        player->renderer_owned_presentation_last_error);
    write_error(error, error_size,
                player->renderer_owned_presentation_last_error.empty()
                    ? "renderer-owned Metal frame refresh failed"
                    : player->renderer_owned_presentation_last_error);
    return -1;
  }
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - profiler_start).count();
  if (macos_profiler_enabled()) {
    spdlog::info(
        "[MacOSProfiler] request_frame_refresh timeout elapsed_ms={} timeout_ms={} attempts={} "
        "baseline_upload={} upload={} baseline_failure={} failures={} clock_us={}",
        elapsed_ms,
        bounded_timeout_ms,
        refresh_attempts,
        baseline_upload_count,
        player->renderer_owned_presentation_upload_count,
        baseline_draw_failure_count,
        player->renderer_owned_presentation_draw_failure_count,
        refresh_clock_us);
  }
  spdlog::warn(
      "[MacOSFrameRefresh] timeout elapsed_ms={} timeout_ms={} attempts={} "
      "min_pts_us={} clock_us={} baseline_upload={} upload={} baseline_failure={} "
      "failures={} deferred_by_backpressure={} last_backpressure_error={} "
      "last_renderer_error={}",
      elapsed_ms,
      bounded_timeout_ms,
      refresh_attempts,
      refresh_min_pts_us,
      refresh_clock_us,
      baseline_upload_count,
      player->renderer_owned_presentation_upload_count,
      baseline_draw_failure_count,
      player->renderer_owned_presentation_draw_failure_count,
      refresh_deferred_by_backpressure,
      last_refresh_backpressure_error,
      last_refresh_renderer_error);
  if (refresh_deferred_by_backpressure &&
      !last_refresh_backpressure_error.empty()) {
    write_error(error, error_size, last_refresh_backpressure_error);
  } else {
    write_error(error, error_size,
                "renderer-owned Metal frame refresh timed out");
  }
  release_manual_refresh_callback_suppression_locked();
  return -2;
}

}  // namespace

int VPMacOSNativePlayerRequestRendererOwnedFrameRefresh(
    VPMacOSNativePlayer* player,
    int32_t timeout_ms,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  return request_renderer_owned_frame_refresh(player, timeout_ms, 0, out, error,
                                              error_size);
}

int VPMacOSNativePlayerRequestRendererOwnedFrameRefreshWithOptions(
    VPMacOSNativePlayer* player,
    int32_t timeout_ms,
    uint32_t flags,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  return request_renderer_owned_frame_refresh(player, timeout_ms, flags, out,
                                              error, error_size);
}

int VPMacOSNativePlayerBakeCurrentFrameSources(
    VPMacOSNativePlayer* player,
    VPMacOSMetalPresentationBackend* backend,
    VPMacOSNativeSourceFrameBakeTarget* targets,
    size_t target_count,
    char* error,
    size_t error_size) {
  if (!player || !backend || !targets || target_count == 0) {
    write_error(error, error_size, "invalid source frame bake arguments");
    return -1;
  }

  std::vector<vr::PresentationSourceFrameTarget> renderer_targets;
  renderer_targets.reserve(target_count);
  for (size_t i = 0; i < target_count; ++i) {
    VPMacOSNativeFrameInfoInit(&targets[i].frame_info);
    targets[i].drawn = 0;
    vr::PresentationSourceFrameTarget target;
    target.output = targets[i].pixel_buffer;
    target.source_slot = targets[i].source_slot;
    target.source_file_id = targets[i].source_file_id;
    target.width = targets[i].width;
    target.height = targets[i].height;
    renderer_targets.push_back(target);
  }

  std::string message;
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    if (!player->renderer_active_locked()) {
      write_error(error, error_size, "renderer is not active");
      return -1;
    }
    ok = player->renderer->draw_current_frame_sources(
        backend->impl,
        renderer_targets.data(),
        renderer_targets.size(),
        &message);
  }
  if (!ok) {
    write_error(error, error_size, message.empty() ? "source frame bake failed" : message);
    return -1;
  }

  int drawn_count = 0;
  for (size_t i = 0; i < target_count; ++i) {
    targets[i].source_file_id = renderer_targets[i].source_file_id;
    targets[i].drawn = renderer_targets[i].drawn;
    copy_frame_info(renderer_targets[i].frame_info, &targets[i].frame_info);
    if (targets[i].drawn) {
      ++drawn_count;
    }
  }
  write_error(error, error_size, "");
  return drawn_count > 0 ? drawn_count : -1;
}
