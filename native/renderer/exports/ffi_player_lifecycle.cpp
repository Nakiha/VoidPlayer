#include "renderer/exports/ffi_player_lifecycle.h"

#include "renderer/exports/ffi_marshalling.h"
#include "renderer/exports/ffi_player_commands.h"
#include "renderer/exports/ffi_player_registry.h"

#include <memory>
#include <mutex>
#include <shared_mutex>

namespace vr::ffi {

naki_vr_status_t copy_global_error_lifecycle_command(naki_vr_player_t /*player*/,
                                                     char* buf,
                                                     size_t cap) {
    copy_error(g_last_error, buf, cap);
    return g_last_error.status;
}

naki_vr_status_t copy_player_error_lifecycle_command(naki_vr_player_t player,
                                                     char* buf,
                                                     size_t cap) {
    auto state = pin_player(player);
    if (!state) {
        copy_error(g_last_error, buf, cap);
        return g_last_error.status;
    }
    std::lock_guard<std::mutex> lock(state->error_mutex);
    copy_error(state->last_error, buf, cap);
    return state->last_error.status;
}

naki_vr_player_t create_player_lifecycle_command() {
    auto player = std::make_shared<PlayerHandleState>();
    auto* handle = player.get();
    register_player(player);
    set_ok();
    return static_cast<naki_vr_player_t>(handle);
}

naki_vr_status_t destroy_player_lifecycle_command(naki_vr_player_t player) {
    if (!player) {
        set_ok();
        return NAKI_VR_OK;
    }
    auto* typed = raw_player(player);
    auto state = unregister_player(typed);
    if (!state) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "player handle is invalid or destroyed");
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    {
        std::unique_lock<std::shared_mutex> gate_lock(state->gate_mutex);
        state->closing = true;
    }
    state->player.shutdown();
    {
        std::lock_guard<std::mutex> error_lock(state->error_mutex);
        state->last_error = {NAKI_VR_OK, ""};
    }
    set_ok();
    return NAKI_VR_OK;
}

naki_vr_status_t initialize_player_v2_lifecycle_command(
    naki_vr_player_t player,
    const naki_vr_player_config_v2_t* config) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!config) {
        set_lease_error(p, NAKI_VR_ERR_INVALID_ARGUMENT, "config is required");
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    RendererConfig cfg;
    if (!fill_renderer_config_v2(*config, cfg)) {
        set_lease_error(p, g_last_error.status, g_last_error.message);
        return g_last_error.status;
    }
    if (!p->initialize(cfg)) {
        set_lease_error(p, NAKI_VR_ERR_OPEN_FAILED, "player initialize failed");
        return NAKI_VR_ERR_OPEN_FAILED;
    }
    set_lease_ok(p);
    return NAKI_VR_OK;
}

int initialize_player_v1_lifecycle_command(naki_vr_player_t player,
                                           const naki_vr_player_config_t* config) {
    auto p = checked_player(player);
    if (!p) return 0;
    if (!config) {
        set_lease_error(p, NAKI_VR_ERR_INVALID_ARGUMENT, "config is required");
        return 0;
    }
    RendererConfig cfg;
    if (!fill_renderer_config_v1(*config, cfg)) {
        set_lease_error(p, g_last_error.status, g_last_error.message);
        return 0;
    }
    if (!p->initialize(cfg)) {
        set_lease_error(p, NAKI_VR_ERR_OPEN_FAILED, "player initialize failed");
        return 0;
    }
    set_lease_ok(p);
    return 1;
}

naki_vr_status_t shutdown_player_lifecycle_command(naki_vr_player_t player) {
    return shutdown_player_command(player);
}

} // namespace vr::ffi
