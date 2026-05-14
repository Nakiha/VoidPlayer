#include "video_renderer/exports/ffi_player_registry.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace vr::ffi {
namespace {

std::mutex g_players_mutex;
std::unordered_map<PlayerHandleState*, std::shared_ptr<PlayerHandleState>> g_live_players;

} // namespace

thread_local FfiError g_last_error;

PlayerHandleState* raw_player(naki_vr_player_t player) {
    return static_cast<PlayerHandleState*>(player);
}

void set_error(naki_vr_status_t status, std::string message) {
    g_last_error.status = status;
    g_last_error.message = std::move(message);
}

void set_ok() {
    set_error(NAKI_VR_OK, "");
}

void copy_error(const FfiError& error, char* buf, size_t cap) {
    if (buf && cap > 0) {
        const size_t to_copy = std::min(cap - 1, error.message.size());
        std::memcpy(buf, error.message.data(), to_copy);
        buf[to_copy] = '\0';
    }
}

void store_state_error_locked(PlayerHandleState& state,
                              naki_vr_status_t status,
                              const std::string& message) {
    state.last_error.status = status;
    state.last_error.message = message;
}

void set_state_error(const std::shared_ptr<PlayerHandleState>& state,
                     naki_vr_status_t status,
                     std::string message) {
    if (state) {
        std::lock_guard<std::mutex> lock(state->error_mutex);
        store_state_error_locked(*state, status, message);
    }
    set_error(status, std::move(message));
}

void set_lease_error(PlayerLease& lease, naki_vr_status_t status, std::string message) {
    set_state_error(lease.state, status, std::move(message));
}

void set_lease_ok(PlayerLease& lease) {
    set_lease_error(lease, NAKI_VR_OK, "");
}

void register_player(const std::shared_ptr<PlayerHandleState>& player) {
    std::lock_guard<std::mutex> lock(g_players_mutex);
    g_live_players.emplace(player.get(), player);
}

std::shared_ptr<PlayerHandleState> unregister_player(PlayerHandleState* player) {
    std::lock_guard<std::mutex> lock(g_players_mutex);
    auto it = g_live_players.find(player);
    if (it == g_live_players.end()) {
        return nullptr;
    }
    auto state = std::move(it->second);
    g_live_players.erase(it);
    return state;
}

std::shared_ptr<PlayerHandleState> pin_player(naki_vr_player_t player) {
    if (!player) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "player is required");
        return nullptr;
    }
    auto* typed = raw_player(player);
    std::lock_guard<std::mutex> lock(g_players_mutex);
    auto it = g_live_players.find(typed);
    if (it == g_live_players.end()) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "player handle is invalid or destroyed");
        return nullptr;
    }
    return it->second;
}

PlayerLease checked_player(naki_vr_player_t player) {
    auto state = pin_player(player);
    if (!state) {
        return {};
    }
    PlayerLease lease;
    lease.state = std::move(state);
    lease.lock = std::shared_lock<std::shared_mutex>(lease.state->gate_mutex);
    if (lease.state->closing) {
        const std::string message = "player handle is invalid or destroyed";
        {
            std::lock_guard<std::mutex> error_lock(lease.state->error_mutex);
            store_state_error_locked(*lease.state, NAKI_VR_ERR_INVALID_ARGUMENT, message);
        }
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, message);
        return {};
    }
    lease.player = &lease.state->player;
    return lease;
}

} // namespace vr::ffi
