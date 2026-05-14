#include "video_renderer/exports/ffi_player_commands.h"

#include "video_renderer/exports/ffi_marshalling.h"
#include "video_renderer/exports/ffi_player_registry.h"
#include "video_renderer/renderer_config_validation.h"

namespace vr::ffi {

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
    p->play();
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t pause_player_command(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    p->pause();
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t seek_player_command(naki_vr_player_t player, int64_t target_pts_us) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    p->seek(target_pts_us);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t seek_typed_player_command(naki_vr_player_t player,
                                           int64_t target_pts_us,
                                           int type) {
    if (!is_valid_seek_type(type)) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "seek type out of range");
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    p->seek(target_pts_us, to_seek_type(type));
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t set_player_speed_command(naki_vr_player_t player, double speed) {
    if (auto result = validate_playback_speed(speed); !result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    p->set_speed(speed);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t set_player_loop_range_command(naki_vr_player_t player,
                                               int enabled,
                                               int64_t start_us,
                                               int64_t end_us) {
    if (auto result = validate_loop_range(enabled != 0, start_us, end_us); !result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    p->set_loop_range(enabled != 0, start_us, end_us);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t set_player_audible_track_command(naki_vr_player_t player,
                                                  int file_id) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    p->set_audible_track(file_id);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t step_player_forward_command(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    p->step_forward();
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t step_player_backward_command(naki_vr_player_t player) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    p->step_backward();
    set_lease_ok(p);
    return NAKI_VR_OK;
}

int query_player_is_playing(naki_vr_player_t player) {
    auto p = checked_player(player);
    return p && p->is_playing() ? 1 : 0;
}

int query_player_is_initialized(naki_vr_player_t player) {
    auto p = checked_player(player);
    return p && p->is_initialized() ? 1 : 0;
}

int64_t query_player_current_pts_us(naki_vr_player_t player) {
    auto p = checked_player(player);
    return p ? p->current_pts_us() : int64_t(0);
}

double query_player_current_speed(naki_vr_player_t player) {
    auto p = checked_player(player);
    return p ? p->current_speed() : 1.0;
}

int query_player_track_count(naki_vr_player_t player) {
    auto p = checked_player(player);
    return p ? static_cast<int>(p->track_count()) : 0;
}

int64_t query_player_duration_us(naki_vr_player_t player) {
    auto p = checked_player(player);
    return p ? p->duration_us() : int64_t(0);
}

int query_player_has_track(naki_vr_player_t player, int slot) {
    auto p = checked_player(player);
    return p && p->has_track(slot) ? 1 : 0;
}

naki_vr_status_t add_player_track_command(naki_vr_player_t player,
                                          const char* video_path,
                                          int* out_slot) {
    if (out_slot) {
        *out_slot = -1;
    }
    if (!video_path) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "video_path is required");
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    if (auto result = validate_renderer_video_path(video_path); !result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
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
    p->remove_track(file_id);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t set_player_track_offset_command(naki_vr_player_t player,
                                                 int file_id,
                                                 int64_t offset_us) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    p->set_track_offset(file_id, offset_us);
    set_lease_ok(p);
    return NAKI_VR_OK;
}

naki_vr_status_t apply_player_layout_command(naki_vr_player_t player,
                                             const naki_vr_player_layout_state_t* state) {
    if (!state) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "layout state is required");
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    if (!validate_ffi_layout_state(*state)) {
        return g_last_error.status;
    }
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    p->apply_layout(to_layout_state(*state));
    set_lease_ok(p);
    return NAKI_VR_OK;
}

void query_player_layout_command(naki_vr_player_t player,
                                 naki_vr_player_layout_state_t* out_state) {
    if (!out_state) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "out_state is required");
        return;
    }
    if (!validate_abi(out_state->size,
                      out_state->abi_version,
                      sizeof(naki_vr_player_layout_state_t),
                      "layout state")) {
        return;
    }
    auto p = checked_player(player);
    if (!p) return;
    fill_ffi_layout_state(p->layout(), *out_state);
    set_ok();
}

} // namespace vr::ffi
