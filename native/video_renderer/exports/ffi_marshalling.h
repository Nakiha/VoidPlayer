#pragma once

#include "media/seek_controller.h"
#include "video_renderer/exports/ffi_exports.h"
#include "video_renderer/layout_state.h"
#include "video_renderer/renderer.h"

#include <cstddef>
#include <cstdint>

namespace vr::ffi {

bool validate_abi(uint32_t size, uint32_t version, size_t expected, const char* name);

bool to_log_config(const naki_vr_log_config_t& c, LogConfig& cfg);

bool fill_renderer_config_v1(const naki_vr_player_config_t& c, RendererConfig& cfg);
bool fill_renderer_config_v2(const naki_vr_player_config_v2_t& c, RendererConfig& cfg);

bool is_valid_seek_type(int type);
SeekType to_seek_type(int type);

LayoutState to_layout_state(const naki_vr_player_layout_state_t& state);
bool validate_ffi_layout_state(const naki_vr_player_layout_state_t& state);
void fill_ffi_layout_state(const LayoutState& layout, naki_vr_player_layout_state_t& out_state);

} // namespace vr::ffi
