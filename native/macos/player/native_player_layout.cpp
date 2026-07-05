#include "native_player_bridge.h"

#include "macos/player/native_player_state.h"
#include "renderer/layout/layout_geometry.h"
#include "renderer/render/shader_constants.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <spdlog/spdlog.h>

using vp_macos::to_layout_state;
using vp_macos::to_native_layout_state;

namespace {

bool macos_profiler_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("VOIDPLAYER_MACOS_PROFILER");
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0;
  }();
  return enabled;
}

}  // namespace

void VPMacOSNativePlayerApplyLayout(VPMacOSNativePlayer* player,
                                    const VPMacOSNativeLayoutState* state) {
  if (!player || !state) {
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  bool playing = false;
  bool active = false;
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    active = true;
    playing = player->renderer->is_playing();
    if (!playing) {
      player->clear_last_frame_locked();
    }
    player->renderer->apply_layout(to_layout_state(*state));
  }
  const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start).count();
  static uint64_t layout_count = 0;
  ++layout_count;
  if (macos_profiler_enabled() && (elapsed_us >= 2000 || layout_count % 120 == 0)) {
    spdlog::info(
        "[MacOSProfiler] apply_layout total_us={} active={} playing={} count={} "
        "zoom={:.3f} offset=({:.3f},{:.3f}) split={:.3f} mode={}",
        elapsed_us,
        active,
        playing,
        layout_count,
        state->zoom_ratio,
        state->view_offset_x,
        state->view_offset_y,
        state->split_pos,
        state->mode);
  }
}

void VPMacOSNativePlayerNoteViewportCompositorActivity(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->note_viewport_compositor_activity();
  }
}

void VPMacOSNativePlayerSetViewportCompositorActive(VPMacOSNativePlayer* player,
                                                    int active) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->set_viewport_compositor_active(active != 0);
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
