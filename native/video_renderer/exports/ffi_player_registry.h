#pragma once

#include "video_renderer/exports/ffi_exports.h"
#include "windows/player/native_player.h"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace vr::ffi {

struct FfiError {
    naki_vr_status_t status = NAKI_VR_OK;
    std::string message;
};

struct PlayerHandleState {
    std::shared_mutex gate_mutex;
    std::mutex error_mutex;
    bool closing = false;
    FfiError last_error;
    NativePlayer player;
};

struct PlayerLease {
    std::shared_ptr<PlayerHandleState> state;
    std::shared_lock<std::shared_mutex> lock;
    NativePlayer* player = nullptr;

    explicit operator bool() const {
        return player != nullptr;
    }

    NativePlayer* operator->() {
        return player;
    }

    const NativePlayer* operator->() const {
        return player;
    }
};

extern thread_local FfiError g_last_error;

PlayerHandleState* raw_player(naki_vr_player_t player);
void set_error(naki_vr_status_t status, std::string message);
void set_ok();
void copy_error(const FfiError& error, char* buf, size_t cap);
void store_state_error_locked(PlayerHandleState& state,
                              naki_vr_status_t status,
                              const std::string& message);
void set_state_error(const std::shared_ptr<PlayerHandleState>& state,
                     naki_vr_status_t status,
                     std::string message);
void set_lease_error(PlayerLease& lease, naki_vr_status_t status, std::string message);
void set_lease_ok(PlayerLease& lease);
void register_player(const std::shared_ptr<PlayerHandleState>& player);
std::shared_ptr<PlayerHandleState> unregister_player(PlayerHandleState* player);
std::shared_ptr<PlayerHandleState> pin_player(naki_vr_player_t player);
PlayerLease checked_player(naki_vr_player_t player);

} // namespace vr::ffi
