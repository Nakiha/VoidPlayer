#include "video_renderer/exports/ffi_player_commands.h"

#include "video_renderer/exports/ffi_marshalling.h"
#include "video_renderer/exports/ffi_player_registry.h"
#include "video_renderer/renderer_config_validation.h"

#include <string>

namespace vr::ffi {
namespace {

bool require_initialized(PlayerLease& lease, const char* operation) {
    if (lease->is_initialized()) {
        return true;
    }
    set_lease_error(
        lease, NAKI_VR_ERR_NOT_INITIALIZED,
        std::string(operation) + " requires an initialized player");
    return false;
}

bool player_has_file_id(PlayerLease& lease, int file_id) {
    for (const auto& track : lease->track_infos()) {
        if (track.file_id == file_id) {
            return true;
        }
    }
    return false;
}

bool require_existing_file_id(PlayerLease& lease, int file_id, const char* operation) {
    if (file_id > 0 && player_has_file_id(lease, file_id)) {
        return true;
    }
    set_lease_error(
        lease, NAKI_VR_ERR_INVALID_ARGUMENT,
        std::string(operation) + " references an unknown track file_id");
    return false;
}

void copy_global_error_to_lease(PlayerLease& lease) {
    set_lease_error(lease, g_last_error.status, g_last_error.message);
}

} // namespace

naki_vr_status_t initialize_player_command(naki_vr_player_t player,
                                           const RendererConfig& config) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!p->initialize(config)) {
        set_lease_error(p, NAKI_VR_ERR_OPEN_FAILED, "player initialize failed");
        return NAKI_VR_ERR_OPEN_FAILED;
    }
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t shutdown_player_command(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    p->shutdown();
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t play_player_command(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!require_initialized(p, "play")) return g_last_error.status;
    p->play();
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t pause_player_command(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!require_initialized(p, "pause")) return g_last_error.status;
    p->pause();
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t seek_player_command(naki_vr_player_t player, int64_t target_pts_us) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!require_initialized(p, "seek")) return g_last_error.status;
    p->seek(target_pts_us);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t seek_typed_player_command(naki_vr_player_t player,
                                           int64_t target_pts_us,
                                           int type) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!is_valid_seek_type(type)) {
        set_lease_error(p, NAKI_VR_ERR_INVALID_ARGUMENT, "seek type out of range");
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    if (!require_initialized(p, "seek")) return g_last_error.status;
    p->seek(target_pts_us, to_seek_type(type));
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t set_player_speed_command(naki_vr_player_t player, double speed) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (auto result = validate_playback_speed(speed); !result.ok) {
        set_lease_error(p, NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    if (!require_initialized(p, "set_speed")) return g_last_error.status;
    p->set_speed(speed);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t set_player_loop_range_command(naki_vr_player_t player,
                                               int enabled,
                                               int64_t start_us,
                                               int64_t end_us) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (auto result = validate_loop_range(enabled != 0, start_us, end_us); !result.ok) {
        set_lease_error(p, NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    if (!require_initialized(p, "set_loop_range")) return g_last_error.status;
    p->set_loop_range(enabled != 0, start_us, end_us);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t set_player_audible_track_command(naki_vr_player_t player,
                                                  int file_id) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!require_initialized(p, "set_audible_track")) return g_last_error.status;
    if (file_id >= 0 && !require_existing_file_id(p, file_id, "set_audible_track")) {
        return g_last_error.status;
    }
    p->set_audible_track(file_id);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t step_player_forward_command(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!require_initialized(p, "step_forward")) return g_last_error.status;
    p->step_forward();
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t step_player_backward_command(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!require_initialized(p, "step_backward")) return g_last_error.status;
    p->step_backward();
    set_lease_ok(p);
    return NAKI_VR_OK;
}

int query_player_is_playing(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return 0;
    const int result = p->is_playing() ? 1 : 0;
    set_lease_ok(p);
    return result;
}

int query_player_is_initialized(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return 0;
    const int result = p->is_initialized() ? 1 : 0;
    set_lease_ok(p);
    return result;
}

int64_t query_player_current_pts_us(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return int64_t(0);
    const int64_t result = p->current_pts_us();
    set_lease_ok(p);
    return result;
}

double query_player_current_speed(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return 1.0;
    const double result = p->current_speed();
    set_lease_ok(p);
    return result;
}

int query_player_track_count(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return 0;
    const int result = static_cast<int>(p->track_count());
    set_lease_ok(p);
    return result;
}

int64_t query_player_duration_us(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return int64_t(0);
    const int64_t result = p->duration_us();
    set_lease_ok(p);
    return result;
}

int query_player_has_track(naki_vr_player_t player, int slot) {
    auto p = checked_player(player);
    if (!p) return 0;
    const int result = p->has_track(slot) ? 1 : 0;
    set_lease_ok(p);
    return result;
}

naki_vr_status_t add_player_track_command(naki_vr_player_t player,
                                          const char* video_path,
                                          int* out_slot) {
    if (out_slot) {
        *out_slot = -1;
    }
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!video_path) {
        set_lease_error(p, NAKI_VR_ERR_INVALID_ARGUMENT, "video_path is required");
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    if (auto result = validate_renderer_video_path(video_path); !result.ok) {
        set_lease_error(p, NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    if (!require_initialized(p, "add_track")) return g_last_error.status;
    int slot = p->add_track(video_path);
    if (slot < 0) {
        set_lease_error(p, NAKI_VR_ERR_OPEN_FAILED, "add_track failed");
        return NAKI_VR_ERR_OPEN_FAILED;
    }
    if (out_slot) {
        *out_slot = slot;
    }
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t remove_player_track_command(naki_vr_player_t player, int file_id) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!require_initialized(p, "remove_track")) return g_last_error.status;
    if (!require_existing_file_id(p, file_id, "remove_track")) {
        return g_last_error.status;
    }
    p->remove_track(file_id);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t set_player_track_offset_command(naki_vr_player_t player,
                                                 int file_id,
                                                 int64_t offset_us) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!require_initialized(p, "set_track_offset")) return g_last_error.status;
    if (!require_existing_file_id(p, file_id, "set_track_offset")) {
        return g_last_error.status;
    }
    p->set_track_offset(file_id, offset_us);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t apply_player_layout_command(naki_vr_player_t player,
                                             const naki_vr_player_layout_state_t* state) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!state) {
        set_lease_error(p, NAKI_VR_ERR_INVALID_ARGUMENT, "layout state is required");
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    if (!validate_ffi_layout_state(*state)) {
        copy_global_error_to_lease(p);
        return g_last_error.status;
    }
    if (!require_initialized(p, "apply_layout")) return g_last_error.status;
    for (int file_id : state->order) {
        if (file_id > 0 && !require_existing_file_id(p, file_id, "apply_layout")) {
            return g_last_error.status;
        }
    }
    p->apply_layout(to_layout_state(*state));
    set_lease_ok(p);
    return NAKI_VR_OK;
}

void query_player_layout_command(naki_vr_player_t player,
                                 naki_vr_player_layout_state_t* out_state) {
    auto p = checked_player(player);
    if (!p) return;
    if (!out_state) {
        set_lease_error(p, NAKI_VR_ERR_INVALID_ARGUMENT, "out_state is required");
        return;
    }
    if (!validate_abi(out_state->size,
                      out_state->abi_version,
                      sizeof(naki_vr_player_layout_state_t),
                      "layout state")) {
        copy_global_error_to_lease(p);
        return;
    }
    fill_ffi_layout_state(p->layout(), *out_state);
    set_lease_ok(p);
}

} // namespace vr::ffi
