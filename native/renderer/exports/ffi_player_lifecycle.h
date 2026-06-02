#pragma once

#include "renderer/exports/ffi_exports.h"

namespace vr::ffi {

naki_vr_status_t copy_global_error_lifecycle_command(naki_vr_player_t player,
                                                     char* buf,
                                                     size_t cap);
naki_vr_status_t copy_player_error_lifecycle_command(naki_vr_player_t player,
                                                     char* buf,
                                                     size_t cap);
naki_vr_player_t create_player_lifecycle_command();
naki_vr_status_t destroy_player_lifecycle_command(naki_vr_player_t player);
naki_vr_status_t initialize_player_v2_lifecycle_command(
    naki_vr_player_t player,
    const naki_vr_player_config_v2_t* config);
int initialize_player_v1_lifecycle_command(naki_vr_player_t player,
                                           const naki_vr_player_config_t* config);
naki_vr_status_t shutdown_player_lifecycle_command(naki_vr_player_t player);

} // namespace vr::ffi
