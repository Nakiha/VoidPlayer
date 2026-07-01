#include "native_player_bridge.h"

#include "macos/metal/metal_presentation_backend_bridge.h"
#include "macos/player/native_player_state.h"
#include "renderer/overlay/analysis_overlay_primitives.h"
#include "renderer/overlay/analysis_overlay_renderer.h"

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

void reset_target_warmup_locked(VPMacOSNativePlayer* player) {
  player->renderer_owned_target_warmup_generation =
      player->presentation_target_generation;
  player->renderer_owned_target_warmup_remaining = 8;
  player->renderer_owned_target_warmup_sample_count = 0;
  player->renderer_owned_target_warmup_last_ms = 0;
  player->renderer_owned_target_warmup_p95_ms = 0;
  player->renderer_owned_target_warmup_intervals_ns.clear();
}

uint32_t pack_overlay_bgra(vr::analysis::OverlayColor color) {
  return static_cast<uint32_t>(color.b) |
         (static_cast<uint32_t>(color.g) << 8) |
         (static_cast<uint32_t>(color.r) << 16) |
         (static_cast<uint32_t>(color.a) << 24);
}

uint32_t pack_overlay_track_payload(int slot, uint8_t line_alpha) {
  return static_cast<uint32_t>(slot & 0xff) |
         (static_cast<uint32_t>(line_alpha) << 8);
}

template <typename Primitive>
VPMacOSNativeOverlayGpuRect pack_overlay_rect(
    const Primitive& primitive,
    int video_width,
    int video_height,
    int slot,
    uint8_t line_alpha,
    bool include_color) {
  VPMacOSNativeOverlayGpuRect rect = {};
  rect.rect_uv0 = vr::pack_overlay_uv16(
      primitive.x0, video_width, primitive.y0, video_height);
  rect.rect_uv1 = vr::pack_overlay_uv16(
      primitive.x1, video_width, primitive.y1, video_height);
  rect.color_bgra = include_color ? pack_overlay_bgra(primitive.color) : 0;
  rect.track_idx = pack_overlay_track_payload(slot, line_alpha);
  return rect;
}

std::string target_address_summary(const std::vector<void*>& targets) {
  std::ostringstream stream;
  for (void* target : targets) {
    stream << "0x" << std::hex << pointer_address(target) << std::dec << ",";
  }
  return stream.str();
}

bool target_ring_changed_locked(VPMacOSNativePlayer* player,
                                VPMacOSMetalPresentationBackend* backend,
                                const std::vector<void*>& targets,
                                int32_t width,
                                int32_t height,
                                int32_t max_track_slots) {
  return player->presentation_target_backend != backend ||
         player->presentation_target_pixel_buffers != targets ||
         player->presentation_target_is_metal_texture ||
         player->presentation_target_width != width ||
         player->presentation_target_height != height ||
         player->presentation_target_max_track_slots != max_track_slots;
}

bool metal_texture_target_changed_locked(VPMacOSNativePlayer* player,
                                         VPMacOSMetalPresentationBackend* backend,
                                         void* texture,
                                         int32_t width,
                                         int32_t height,
                                         uint64_t pixel_format,
                                         int32_t max_track_slots,
                                         float viewport_left,
                                         float viewport_top,
                                         float viewport_right,
                                         float viewport_bottom) {
  return player->presentation_target_backend != backend ||
         player->presentation_target_pixel_buffer == nullptr ||
         player->presentation_target_pixel_buffer != texture ||
         player->presentation_target_pixel_buffers.empty() ||
         !player->presentation_target_is_metal_texture ||
         player->presentation_target_pixel_format != pixel_format ||
         player->presentation_target_width != width ||
         player->presentation_target_height != height ||
         player->presentation_target_max_track_slots != max_track_slots ||
         player->presentation_target_viewport_left != viewport_left ||
         player->presentation_target_viewport_top != viewport_top ||
         player->presentation_target_viewport_right != viewport_right ||
         player->presentation_target_viewport_bottom != viewport_bottom;
}

void commit_target_ring_locked(VPMacOSNativePlayer* player,
                               VPMacOSMetalPresentationBackend* backend,
                               const std::vector<void*>& targets,
                               void* displayed_pixel_buffer,
                               void* protected_pixel_buffer,
                               int32_t width,
                               int32_t height,
                               int32_t max_track_slots,
                               bool target_changed) {
  player->presentation_target_backend = backend;
  player->presentation_target_pixel_buffer = targets.empty() ? nullptr : targets.front();
  player->presentation_target_pixel_buffers = targets;
  player->presentation_target_is_metal_texture = false;
  player->presentation_target_pixel_format = 0;
  player->presentation_target_width = width;
  player->presentation_target_height = height;
  player->presentation_target_max_track_slots = max_track_slots;
  player->presentation_target_viewport_left = 0.0f;
  player->presentation_target_viewport_top = 0.0f;
  player->presentation_target_viewport_right = 1.0f;
  player->presentation_target_viewport_bottom = 1.0f;
  if (!target_changed) {
    return;
  }
  ++player->presentation_target_generation;
  reset_target_warmup_locked(player);
  spdlog::info(
      "[MacOSFrameRefresh] install_target_ring generation={} targets=[{}] "
      "displayed=0x{:x} protected=0x{:x} size={}x{} slots={}",
      player->presentation_target_generation,
      target_address_summary(targets),
      pointer_address(displayed_pixel_buffer),
      pointer_address(protected_pixel_buffer),
      width,
      height,
      max_track_slots);
  player->last_renderer_owned_presentation_succeeded = false;
  player->last_renderer_owned_frame_info_available = false;
  player->last_renderer_owned_frame_info = {};
  player->renderer_owned_presentation_consecutive_failures = 0;
  player->renderer_owned_presentation_last_error.clear();
}

void commit_metal_texture_target_locked(VPMacOSNativePlayer* player,
                                        VPMacOSMetalPresentationBackend* backend,
                                        void* texture,
                                        int32_t width,
                                        int32_t height,
                                        uint64_t pixel_format,
                                        int32_t max_track_slots,
                                        float viewport_left,
                                        float viewport_top,
                                        float viewport_right,
                                        float viewport_bottom,
                                        bool target_changed) {
  const bool target_config_changed =
      player->presentation_target_backend != backend ||
      !player->presentation_target_is_metal_texture ||
      player->presentation_target_pixel_format != pixel_format ||
      player->presentation_target_width != width ||
      player->presentation_target_height != height ||
      player->presentation_target_max_track_slots != max_track_slots ||
      player->presentation_target_viewport_left != viewport_left ||
      player->presentation_target_viewport_top != viewport_top ||
      player->presentation_target_viewport_right != viewport_right ||
      player->presentation_target_viewport_bottom != viewport_bottom;
  std::vector<void*> targets = {texture};
  player->presentation_target_backend = backend;
  player->presentation_target_pixel_buffer = texture;
  player->presentation_target_pixel_buffers = targets;
  player->presentation_target_is_metal_texture = true;
  player->presentation_target_pixel_format = pixel_format;
  player->presentation_target_width = width;
  player->presentation_target_height = height;
  player->presentation_target_max_track_slots = max_track_slots;
  player->presentation_target_viewport_left = viewport_left;
  player->presentation_target_viewport_top = viewport_top;
  player->presentation_target_viewport_right = viewport_right;
  player->presentation_target_viewport_bottom = viewport_bottom;
  if (!target_changed) {
    return;
  }
  ++player->presentation_target_generation;
  if (target_config_changed) {
    reset_target_warmup_locked(player);
    spdlog::info(
        "[MacOSFrameRefresh] install_metal_texture_target generation={} "
        "target=0x{:x} size={}x{} format={} slots={} viewport=({:.4f},{:.4f})-({:.4f},{:.4f})",
        player->presentation_target_generation,
        pointer_address(texture),
        width,
        height,
        pixel_format,
        max_track_slots,
        viewport_left,
        viewport_top,
        viewport_right,
        viewport_bottom);
  } else {
    spdlog::debug(
        "[MacOSFrameRefresh] install_metal_texture_target generation={} target=0x{:x}",
        player->presentation_target_generation,
        pointer_address(texture));
  }
  player->last_renderer_owned_presentation_succeeded = false;
  player->last_renderer_owned_frame_info_available = false;
  player->last_renderer_owned_frame_info = {};
  player->renderer_owned_presentation_consecutive_failures = 0;
  player->renderer_owned_presentation_last_error.clear();
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
    target_changed = target_ring_changed_locked(
        player, backend, targets, width, height, clamped_track_slots);
  }

  std::string renderer_error;
  bool renderer_was_active = false;
  bool renderer_install_failed = false;
  {
    std::lock_guard<std::mutex> player_lock(player->mutex);
    renderer_was_active = player->renderer_active_locked();
    if (renderer_was_active) {
      if (!target_changed) {
        if (displayed_pixel_buffer) {
          player->renderer->mark_headless_output_displayed(displayed_pixel_buffer);
        }
        player->renderer->protect_headless_output(protected_pixel_buffer);
        return 0;
      }
      if (!player->renderer->install_headless_output_ring(pixel_buffers,
                                                          pixel_buffer_count,
                                                          displayed_pixel_buffer,
                                                          protected_pixel_buffer,
                                                          width,
                                                          height,
                                                          clamped_track_slots)) {
        renderer_install_failed = true;
        renderer_error = player->renderer->presentation_backend_last_error();
      }
      if (renderer_install_failed && renderer_error.empty()) {
        renderer_error = "failed to install renderer-owned Metal presentation target ring";
      }
    }
  }
  if (renderer_install_failed) {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    player->record_presentation_failure_locked(renderer_error, true);
    player->presentation_condition.notify_all();
    return -1;
  }
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    commit_target_ring_locked(player,
                              backend,
                              targets,
                              displayed_pixel_buffer,
                              protected_pixel_buffer,
                              width,
                              height,
                              clamped_track_slots,
                              target_changed);
  }
  if (target_changed) {
    player->presentation_condition.notify_all();
  }
  if (renderer_was_active) {
    return 0;
  }

  {
    std::lock_guard<std::mutex> player_lock(player->mutex);
    if (!player->opened_path.empty()) {
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
        std::string install_error =
            player->renderer->presentation_backend_last_error();
        if (install_error.empty()) {
          install_error =
              "failed to install renderer-owned Metal presentation target ring";
        }
        player->record_presentation_failure_locked(install_error, true);
        player->presentation_condition.notify_all();
        return -1;
      }
    }
  }
  return 0;
}

int VPMacOSNativePlayerInstallMetalDrawableTarget(
    VPMacOSNativePlayer* player,
    VPMacOSMetalPresentationBackend* backend,
    void* mtl_texture,
    int32_t width,
    int32_t height,
    uint64_t pixel_format,
    int32_t max_track_slots,
    float viewport_left,
    float viewport_top,
    float viewport_right,
    float viewport_bottom) {
  if (!player || !backend || !mtl_texture || width <= 0 || height <= 0) {
    return -1;
  }
  viewport_left = std::clamp(viewport_left, 0.0f, 1.0f);
  viewport_top = std::clamp(viewport_top, 0.0f, 1.0f);
  viewport_right = std::clamp(viewport_right, viewport_left, 1.0f);
  viewport_bottom = std::clamp(viewport_bottom, viewport_top, 1.0f);
  if (viewport_right <= viewport_left || viewport_bottom <= viewport_top) {
    return -1;
  }
  const int32_t clamped_track_slots =
      std::clamp(max_track_slots, static_cast<int32_t>(1),
                 static_cast<int32_t>(VPMacOSNativeMaxTracks));
  bool target_changed = false;
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    target_changed = metal_texture_target_changed_locked(player,
                                                         backend,
                                                         mtl_texture,
                                                         width,
                                                         height,
                                                         pixel_format,
                                                         clamped_track_slots,
                                                         viewport_left,
                                                         viewport_top,
                                                         viewport_right,
                                                         viewport_bottom);
  }

  std::string renderer_error;
  bool renderer_was_active = false;
  bool renderer_install_failed = false;
  {
    std::lock_guard<std::mutex> player_lock(player->mutex);
    renderer_was_active = player->renderer_active_locked();
    if (renderer_was_active) {
      vr::PresentationExternalMetalRenderTarget target;
      target.texture = mtl_texture;
      target.width = width;
      target.height = height;
      target.pixel_format = pixel_format;
      target.max_track_slots = clamped_track_slots;
      target.viewport_left = viewport_left;
      target.viewport_top = viewport_top;
      target.viewport_right = viewport_right;
      target.viewport_bottom = viewport_bottom;
      if (!player->renderer->install_headless_metal_texture_output(target)) {
        renderer_install_failed = true;
        renderer_error = player->renderer->presentation_backend_last_error();
      }
      if (renderer_install_failed && renderer_error.empty()) {
        renderer_error = "failed to install renderer-owned Metal texture target";
      }
    }
  }
  if (renderer_install_failed) {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    player->record_presentation_failure_locked(renderer_error, true);
    player->presentation_condition.notify_all();
    return -1;
  }
  {
    std::lock_guard<std::mutex> lock(player->callback_mutex);
    commit_metal_texture_target_locked(player,
                                       backend,
                                       mtl_texture,
                                       width,
                                       height,
                                       pixel_format,
                                       clamped_track_slots,
                                       viewport_left,
                                       viewport_top,
                                       viewport_right,
                                       viewport_bottom,
                                       target_changed);
  }
  if (target_changed) {
    player->presentation_condition.notify_all();
  }
  if (renderer_was_active) {
    return 0;
  }

  {
    std::lock_guard<std::mutex> player_lock(player->mutex);
    if (!player->opened_path.empty()) {
      if (!player->ensure_renderer_locked(renderer_error)) {
        std::lock_guard<std::mutex> lock(player->callback_mutex);
        player->record_presentation_failure_locked(renderer_error, true);
        player->presentation_condition.notify_all();
        return -1;
      }
    }
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
    player->presentation_target_is_metal_texture = false;
    player->presentation_target_pixel_format = 0;
    player->presentation_target_width = 0;
    player->presentation_target_height = 0;
    player->presentation_target_max_track_slots = 1;
    ++player->presentation_target_generation;
    spdlog::info(
        "[MacOSFrameRefresh] clear_target generation={} upload={} failures={}",
        player->presentation_target_generation,
        player->renderer_owned_presentation_upload_count,
        player->renderer_owned_presentation_draw_failure_count);
    player->last_renderer_owned_presentation_succeeded = false;
    player->last_renderer_owned_frame_info_available = false;
    player->last_renderer_owned_frame_info = {};
    player->renderer_owned_presentation_consecutive_failures = 0;
    player->renderer_owned_presentation_last_error.clear();
  }
  player->presentation_condition.notify_all();
  std::lock_guard<std::mutex> player_lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->clear_headless_output();
  }
}

int VPMacOSNativePlayerUpdateExternalFlutterSurface(
    VPMacOSNativePlayer* player,
    void* mtl_texture,
    int32_t width,
    int32_t height,
    uint64_t pixel_format,
    uint64_t frame_generation) {
  if (!player || !mtl_texture || width <= 0 || height <= 0) {
    return -1;
  }
  vr::PresentationExternalMetalSurface surface;
  surface.texture = mtl_texture;
  surface.width = width;
  surface.height = height;
  surface.pixel_format = pixel_format;
  surface.frame_generation = frame_generation;
  std::lock_guard<std::mutex> player_lock(player->mutex);
  if (!player->renderer_active_locked()) {
    return 0;
  }
  return player->renderer->update_external_flutter_metal_surface(surface) ? 0 : -1;
}

void VPMacOSNativePlayerClearExternalFlutterSurface(
    VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> player_lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->clear_external_flutter_metal_surface();
  }
}

int VPMacOSNativePlayerUpdateSourceProjection(
    VPMacOSNativePlayer* player,
    int32_t mode,
    float split_pos,
    int32_t active_track_count,
    const int32_t* source_order,
    const float* display_offset_x,
    const float* display_offset_y,
    const float* inv_display_size_x,
    const float* inv_display_size_y,
    const float* view_offset_uv_x,
    const float* view_offset_uv_y,
    size_t count) {
  if (!player || count == 0) {
    return -1;
  }
  vr::PresentationSourceProjection projection;
  projection.enabled = true;
  projection.mode = mode;
  projection.split_pos = std::clamp(split_pos, 0.0f, 1.0f);
  projection.active_track_count = std::clamp(active_track_count, 1, 4);
  for (size_t i = 0; i < projection.source_order.size(); ++i) {
    const int fallback_order = static_cast<int>(i);
    projection.source_order[i] =
        source_order && i < count ? source_order[i] : fallback_order;
    projection.display_offset_x[i] =
        display_offset_x && i < count ? display_offset_x[i] : 0.0f;
    projection.display_offset_y[i] =
        display_offset_y && i < count ? display_offset_y[i] : 0.0f;
    projection.inv_display_size_x[i] =
        inv_display_size_x && i < count ? inv_display_size_x[i] : 0.0f;
    projection.inv_display_size_y[i] =
        inv_display_size_y && i < count ? inv_display_size_y[i] : 0.0f;
    projection.view_offset_uv_x[i] =
        view_offset_uv_x && i < count ? view_offset_uv_x[i] : 0.0f;
    projection.view_offset_uv_y[i] =
        view_offset_uv_y && i < count ? view_offset_uv_y[i] : 0.0f;
  }
  std::lock_guard<std::mutex> player_lock(player->mutex);
  if (!player->renderer_active_locked()) {
    return 0;
  }
  return player->renderer->update_source_projection(projection) ? 0 : -1;
}

void VPMacOSNativePlayerClearSourceProjection(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> player_lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->clear_source_projection();
  }
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
  bool baseline_target_is_metal_texture = false;
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
    baseline_target_is_metal_texture = player->presentation_target_is_metal_texture;
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
    if (baseline_target_is_metal_texture) {
      return frame_target == baseline_target_address;
    }
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
      const auto wait_slice = std::min<std::chrono::milliseconds>(
          std::max<std::chrono::milliseconds>(remaining,
                                              std::chrono::milliseconds(1)),
          std::chrono::milliseconds(20));
      const auto wait_deadline = std::min(deadline, now + wait_slice);
      player->presentation_condition.wait_until(callback_lock, wait_deadline,
                                                completed);
      if (completed()) {
        break;
      }
      const bool observed_mismatched_target =
          player->renderer_owned_presentation_upload_count >
              baseline_upload_count &&
          !frame_matches_target_request();
      if (!refresh_submitted || observed_mismatched_target) {
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
      "last_renderer_error={} baseline_target=0x{:x} last_frame_target=0x{:x} "
      "baseline_target_is_metal={} baseline_target_generation={} current_target_generation={}",
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
      last_refresh_renderer_error,
      baseline_target_address,
      player->last_renderer_owned_frame_info_available
          ? static_cast<uint64_t>(player->last_renderer_owned_frame_info.target_pixel_buffer_address)
          : 0,
      baseline_target_is_metal_texture,
      baseline_target_generation,
      player->presentation_target_generation);
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
  const VPMacOSNativeSourceFrameBakeTarget* initial_target = nullptr;
  for (size_t i = 0; i < target_count; ++i) {
    if (targets[i].pixel_buffer && targets[i].width > 0 && targets[i].height > 0) {
      initial_target = &targets[i];
      break;
    }
  }
  if (!initial_target) {
    write_error(error, error_size, "source frame bake target list has no valid pixel buffer");
    return -1;
  }
  std::shared_ptr<vr::PresentationBackend> source_bake_backend =
      VPMacOSMetalPresentationBackendSourceBakeBackend(
          backend,
          initial_target->pixel_buffer,
          initial_target->width,
          initial_target->height,
          error,
          error_size);
  if (!source_bake_backend) {
    return -1;
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
        *source_bake_backend,
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

int VPMacOSNativePlayerCopyCurrentOverlayPrimitives(
    VPMacOSNativePlayer* player,
    VPMacOSNativeOverlayPrimitiveSnapshot* snapshot,
    VPMacOSNativeOverlayGpuRect* fill_rects,
    size_t fill_rect_capacity,
    VPMacOSNativeOverlayGpuRect* line_rects,
    size_t line_rect_capacity,
    VPMacOSNativeOverlayGpuRect* motion_lines,
    size_t motion_line_capacity,
    char* error,
    size_t error_size) {
  if (!player || !snapshot) {
    write_error(error, error_size, "invalid overlay primitive copy arguments");
    return -1;
  }

  VPMacOSNativeOverlayPrimitiveSnapshotInit(snapshot);
  std::shared_ptr<const vr::AnalysisOverlayPrimitivePackage> package;
  std::string message;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    if (!player->renderer_active_locked()) {
      write_error(error, error_size, "renderer is not active");
      return -1;
    }
    package = player->renderer->current_overlay_primitives(&message);
  }
  if (!message.empty()) {
    write_error(error, error_size, message);
    return -1;
  }
  if (package) {
    snapshot->generation = package->cache_generation;
    snapshot->overlay_track_count = package->overlay_track_count;
    snapshot->matched_track_count = package->matched_track_count;
    snapshot->missing_track_slot_count = package->missing_track_slot_count;
    snapshot->missing_presented_frame_count =
        package->missing_presented_frame_count;
    snapshot->missing_frame_index_count = package->missing_frame_index_count;
    snapshot->invalid_video_size_count = package->invalid_video_size_count;
    snapshot->overlay_frame_missing_count = package->overlay_frame_missing_count;
    snapshot->heatmap_missing_feature_track_count =
        package->heatmap_missing_feature_track_count;
  }
  if (!package || package->empty()) {
    write_error(error, error_size, "");
    return 0;
  }

  for (const auto& track : package->tracks) {
    snapshot->fill_rect_count += track.fill_rects.size();
    snapshot->line_rect_count += track.outline_rects.size();
    snapshot->motion_line_count += track.motion_lines.size();
  }

  const bool capacity_ok =
      fill_rect_capacity >= snapshot->fill_rect_count &&
      line_rect_capacity >= snapshot->line_rect_count &&
      motion_line_capacity >= snapshot->motion_line_count;
  if (!capacity_ok) {
    write_error(error, error_size, "overlay primitive buffers are too small");
    return -2;
  }

  size_t fill_index = 0;
  size_t line_index = 0;
  size_t motion_index = 0;
  for (const auto& track : package->tracks) {
    if (track.video_width <= 0 || track.video_height <= 0) {
      continue;
    }
    for (const auto& primitive : track.fill_rects) {
      if (fill_rects && fill_index < fill_rect_capacity) {
        fill_rects[fill_index] = pack_overlay_rect(
            primitive,
            track.video_width,
            track.video_height,
            track.slot,
            track.line_alpha,
            true);
      }
      ++fill_index;
    }
    for (const auto& primitive : track.outline_rects) {
      if (line_rects && line_index < line_rect_capacity) {
        line_rects[line_index] = pack_overlay_rect(
            primitive,
            track.video_width,
            track.video_height,
            track.slot,
            track.line_alpha,
            false);
      }
      ++line_index;
    }
    for (const auto& primitive : track.motion_lines) {
      if (motion_lines && motion_index < motion_line_capacity) {
        motion_lines[motion_index] = pack_overlay_rect(
            primitive,
            track.video_width,
            track.video_height,
            track.slot,
            track.line_alpha,
            true);
      }
      ++motion_index;
    }
  }

  write_error(error, error_size, "");
  return 0;
}
