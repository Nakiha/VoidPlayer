#pragma once

#include "renderer/exports/ffi_exports.h"
#include "renderer/renderer.h"

#include <cstdint>

namespace vr::ffi {

naki_vr_status_t initialize_player_command(naki_vr_player_t player,
                                           const RendererConfig& config);
naki_vr_status_t shutdown_player_command(naki_vr_player_t player);

naki_vr_status_t play_player_command(naki_vr_player_t player);
naki_vr_status_t pause_player_command(naki_vr_player_t player);
naki_vr_status_t seek_player_command(naki_vr_player_t player, int64_t target_pts_us);
naki_vr_status_t seek_typed_player_command(naki_vr_player_t player,
                                           int64_t target_pts_us,
                                           int type);
naki_vr_status_t set_player_speed_command(naki_vr_player_t player, double speed);
naki_vr_status_t set_player_loop_range_command(naki_vr_player_t player,
                                               int enabled,
                                               int64_t start_us,
                                               int64_t end_us);
naki_vr_status_t set_player_audible_track_command(naki_vr_player_t player,
                                                  int file_id);
naki_vr_status_t step_player_forward_command(naki_vr_player_t player);
naki_vr_status_t step_player_backward_command(naki_vr_player_t player);

int query_player_is_playing(naki_vr_player_t player);
int query_player_is_initialized(naki_vr_player_t player);
int64_t query_player_current_pts_us(naki_vr_player_t player);
double query_player_current_speed(naki_vr_player_t player);
int query_player_track_count(naki_vr_player_t player);
int64_t query_player_duration_us(naki_vr_player_t player);
int query_player_has_track(naki_vr_player_t player, int slot);

naki_vr_status_t add_player_track_command(naki_vr_player_t player,
                                          const char* video_path,
                                          int* out_slot);
naki_vr_status_t remove_player_track_command(naki_vr_player_t player, int file_id);
naki_vr_status_t set_player_track_offset_command(naki_vr_player_t player,
                                                 int file_id,
                                                 int64_t offset_us);

naki_vr_status_t apply_player_layout_command(naki_vr_player_t player,
                                             const naki_vr_player_layout_state_t* state);
void query_player_layout_command(naki_vr_player_t player,
                                 naki_vr_player_layout_state_t* out_state);

} // namespace vr::ffi
