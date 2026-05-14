#include "video_renderer/exports/ffi_exports.h"
#include "player/native_player.h"
#include "video_renderer/layout_validation.h"
#include "video_renderer/renderer_config_validation.h"
#include "video_renderer/renderer_limits.h"
#include "common/logging.h"
#include "common/windows_crash_handler.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct FfiError {
    naki_vr_status_t status = NAKI_VR_OK;
    std::string message;
};

thread_local FfiError g_last_error;
std::mutex g_players_mutex;
struct PlayerHandleState {
    std::shared_mutex gate_mutex;
    std::mutex error_mutex;
    bool closing = false;
    FfiError last_error;
    vr::NativePlayer player;
};

std::unordered_map<PlayerHandleState*, std::shared_ptr<PlayerHandleState>> g_live_players;

struct PlayerLease {
    std::shared_ptr<PlayerHandleState> state;
    std::shared_lock<std::shared_mutex> lock;
    vr::NativePlayer* player = nullptr;

    explicit operator bool() const {
        return player != nullptr;
    }

    vr::NativePlayer* operator->() {
        return player;
    }

    const vr::NativePlayer* operator->() const {
        return player;
    }
};

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

bool validate_abi(uint32_t size, uint32_t version, size_t expected, const char* name) {
    if (size < expected) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, std::string(name) + " size is too small");
        return false;
    }
    if (version != NAKI_VR_ABI_VERSION) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, std::string(name) + " ABI version mismatch");
        return false;
    }
    return true;
}

bool validate_log_config(const naki_vr_log_config_t& c) {
    if (!validate_abi(c.size, c.abi_version, sizeof(naki_vr_log_config_t), "log config")) {
        return false;
    }
    if (c.level < NAKI_VR_LOG_TRACE || c.level > NAKI_VR_LOG_OFF) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "log level out of range");
        return false;
    }
    if (c.max_files < 0) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "log max_files must be non-negative");
        return false;
    }
    return true;
}

bool to_log_config(const naki_vr_log_config_t& c, vr::LogConfig& cfg) {
    if (!validate_log_config(c)) {
        return false;
    }
    cfg.pattern = c.pattern ? c.pattern : "";
    cfg.file_path = c.file_path ? c.file_path : "";
    cfg.max_file_size = c.max_file_size;
    cfg.max_files = c.max_files;
    cfg.level = static_cast<spdlog::level::level_enum>(c.level);
    return true;
}

bool validate_player_config(const naki_vr_player_config_t& c) {
    if (!validate_abi(c.size, c.abi_version, sizeof(naki_vr_player_config_t), "player config")) {
        return false;
    }
    if (auto result = vr::validate_renderer_dimensions(c.width, c.height, "player dimensions");
        !result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return false;
    }
    return validate_log_config(c.log_config);
}

bool validate_player_config_v2(const naki_vr_player_config_v2_t& c) {
    if (!validate_abi(c.size, c.abi_version, sizeof(naki_vr_player_config_v2_t),
                      "player config v2")) {
        return false;
    }
    if (c.flags != 0 || c.reserved0 != 0) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "player config v2 reserved fields must be zero");
        return false;
    }
    for (uint64_t value : c.reserved) {
        if (value != 0) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT,
                      "player config v2 reserved fields must be zero");
            return false;
        }
    }
    if (auto result = vr::validate_renderer_dimensions(c.width, c.height, "player dimensions");
        !result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return false;
    }
    if (c.video_path_count > 0 && !c.video_paths) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT,
                  "video_paths is required when video_path_count is non-zero");
        return false;
    }
    if (c.video_path_count > vr::kMaxRendererVideoPaths) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "too many video paths");
        return false;
    }
    return validate_log_config(c.log_config);
}

bool is_valid_seek_type(int type) {
    return type == NAKI_VR_SEEK_KEYFRAME || type == NAKI_VR_SEEK_EXACT;
}

vr::LayoutState to_layout_state(const naki_vr_player_layout_state_t& state) {
    vr::LayoutState layout;
    layout.mode = state.mode;
    layout.split_pos = state.split_pos;
    layout.zoom_ratio = state.zoom_ratio;
    layout.view_offset[0] = state.view_offset[0];
    layout.view_offset[1] = state.view_offset[1];
    layout.pixel_size_mode = state.pixel_size_mode;
    for (int i = 0; i < 4; ++i) layout.order[i] = state.order[i];
    return layout;
}

bool validate_ffi_layout_state(const naki_vr_player_layout_state_t& state) {
    if (!validate_abi(state.size,
                      state.abi_version,
                      sizeof(naki_vr_player_layout_state_t),
                      "layout state")) {
        return false;
    }
    const auto result = vr::validate_layout_state(to_layout_state(state));
    if (!result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return false;
    }
    return true;
}

bool fill_renderer_config_v1(const naki_vr_player_config_t& c, vr::RendererConfig& cfg) {
    if (!validate_player_config(c)) {
        return false;
    }
    cfg.hwnd = reinterpret_cast<void*>(c.hwnd);
    cfg.width = c.width;
    cfg.height = c.height;
    cfg.use_hardware_decode = c.use_hardware_decode != 0;
    if (!to_log_config(c.log_config, cfg.log_config)) {
        return false;
    }

    if (c.video_paths) {
        size_t path_count = 0;
        for (; path_count <= vr::kMaxRendererVideoPaths; ++path_count) {
            const char* path = c.video_paths[path_count];
            if (!path) {
                break;
            }
            cfg.video_paths.emplace_back(path);
        }
        if (path_count > vr::kMaxRendererVideoPaths) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT,
                      "too many video paths or missing null terminator");
            return false;
        }
    }

    if (auto result = vr::validate_renderer_config(cfg); !result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return false;
    }
    return true;
}

bool fill_renderer_config_v2(const naki_vr_player_config_v2_t& c, vr::RendererConfig& cfg) {
    if (!validate_player_config_v2(c)) {
        return false;
    }
    cfg.hwnd = reinterpret_cast<void*>(c.hwnd);
    cfg.width = c.width;
    cfg.height = c.height;
    cfg.use_hardware_decode = c.use_hardware_decode != 0;
    if (!to_log_config(c.log_config, cfg.log_config)) {
        return false;
    }
    for (size_t i = 0; i < c.video_path_count; ++i) {
        const char* path = c.video_paths[i];
        if (!path) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "video path must not be null");
            return false;
        }
        cfg.video_paths.emplace_back(path);
    }
    if (auto result = vr::validate_renderer_config(cfg); !result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return false;
    }
    return true;
}

naki_vr_status_t initialize_player_with_config(naki_vr_player_t player,
                                               const vr::RendererConfig& cfg) {
    auto p = checked_player(player);
    if (!p) return g_last_error.status;
    if (!p->initialize(cfg)) {
        set_lease_error(p, NAKI_VR_ERR_OPEN_FAILED, "player initialize failed");
        return NAKI_VR_ERR_OPEN_FAILED;
    }
    set_lease_ok(p);
    return NAKI_VR_OK;
}

template <typename Fn, typename T>
T ffi_guard(const char* name, T fallback, Fn&& fn) noexcept {
    try {
        return std::forward<Fn>(fn)();
    } catch (const std::exception& e) {
        spdlog::error("{} exception: {}", name, e.what());
        set_error(NAKI_VR_ERR_INTERNAL, std::string(name) + " exception: " + e.what());
        return fallback;
    } catch (...) {
        spdlog::error("{} unknown exception", name);
        set_error(NAKI_VR_ERR_INTERNAL, std::string(name) + " unknown exception");
        return fallback;
    }
}

template <typename Fn>
void ffi_guard_void(const char* name, Fn&& fn) noexcept {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::exception& e) {
        spdlog::error("{} exception: {}", name, e.what());
        set_error(NAKI_VR_ERR_INTERNAL, std::string(name) + " exception: " + e.what());
    } catch (...) {
        spdlog::error("{} unknown exception", name);
        set_error(NAKI_VR_ERR_INTERNAL, std::string(name) + " unknown exception");
    }
}

} // namespace

/* ---- Lifecycle ---- */

uint32_t naki_vr_abi_version(void) noexcept {
    return NAKI_VR_ABI_VERSION;
}

naki_vr_status_t naki_vr_last_error(naki_vr_player_t /*player*/, char* buf, size_t cap) noexcept {
    copy_error(g_last_error, buf, cap);
    return g_last_error.status;
}

naki_vr_status_t naki_vr_player_get_error(naki_vr_player_t player, char* buf, size_t cap) noexcept {
    return ffi_guard("naki_vr_player_get_error", NAKI_VR_ERR_INTERNAL, [player, buf, cap]() {
        auto state = pin_player(player);
        if (!state) {
            copy_error(g_last_error, buf, cap);
            return g_last_error.status;
        }
        std::lock_guard<std::mutex> lock(state->error_mutex);
        copy_error(state->last_error, buf, cap);
        return state->last_error.status;
    });
}

naki_vr_player_t naki_vr_player_create(void) noexcept {
    return ffi_guard("naki_vr_player_create", static_cast<naki_vr_player_t>(nullptr), []() {
        auto player = std::make_shared<PlayerHandleState>();
        auto* handle = player.get();
        register_player(player);
        set_ok();
        return static_cast<naki_vr_player_t>(handle);
    });
}

naki_vr_status_t naki_vr_player_destroy_status(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_destroy_status", NAKI_VR_ERR_INTERNAL, [player]() {
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
    });
}

void naki_vr_player_destroy(naki_vr_player_t player) noexcept {
    (void)naki_vr_player_destroy_status(player);
}

naki_vr_status_t naki_vr_player_initialize_v2(naki_vr_player_t player, const naki_vr_player_config_v2_t* config) noexcept {
    return ffi_guard("naki_vr_player_initialize_v2", NAKI_VR_ERR_INTERNAL, [player, config]() {
        if (!config) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "config is required");
            return NAKI_VR_ERR_INVALID_ARGUMENT;
        }
        vr::RendererConfig cfg;
        if (!fill_renderer_config_v2(*config, cfg)) {
            return g_last_error.status;
        }
        return initialize_player_with_config(player, cfg);
    });
}

naki_vr_status_t naki_vr_player_shutdown_status(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_shutdown_status", NAKI_VR_ERR_INTERNAL, [player]() {
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        p->shutdown();
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

int naki_vr_player_initialize(naki_vr_player_t player, const naki_vr_player_config_t* config) noexcept {
    return ffi_guard("naki_vr_player_initialize", 0, [player, config]() {
        if (!config) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "config is required");
            return 0;
        }
        vr::RendererConfig cfg;
        if (!fill_renderer_config_v1(*config, cfg)) {
            return 0;
        }
        return initialize_player_with_config(player, cfg) == NAKI_VR_OK ? 1 : 0;
    });
}

void naki_vr_player_shutdown(naki_vr_player_t player) noexcept {
    (void)naki_vr_player_shutdown_status(player);
}

/* ---- Playback ---- */

void naki_vr_player_play(naki_vr_player_t player) noexcept {
    (void)naki_vr_player_play_status(player);
}

naki_vr_status_t naki_vr_player_play_status(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_play_status", NAKI_VR_ERR_INTERNAL, [player]() {
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        p->play();
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

void naki_vr_player_pause(naki_vr_player_t player) noexcept {
    (void)naki_vr_player_pause_status(player);
}

naki_vr_status_t naki_vr_player_pause_status(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_pause_status", NAKI_VR_ERR_INTERNAL, [player]() {
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        p->pause();
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

void naki_vr_player_seek(naki_vr_player_t player, int64_t target_pts_us) noexcept {
    (void)naki_vr_player_seek_status(player, target_pts_us);
}

naki_vr_status_t naki_vr_player_seek_status(naki_vr_player_t player, int64_t target_pts_us) noexcept {
    return ffi_guard("naki_vr_player_seek_status", NAKI_VR_ERR_INTERNAL, [player, target_pts_us]() {
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        p->seek(target_pts_us);
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

void naki_vr_player_seek_typed(naki_vr_player_t player, int64_t target_pts_us, int type) noexcept {
    (void)naki_vr_player_seek_typed_status(player, target_pts_us, type);
}

naki_vr_status_t naki_vr_player_seek_typed_status(naki_vr_player_t player, int64_t target_pts_us, int type) noexcept {
    return ffi_guard("naki_vr_player_seek_typed_status", NAKI_VR_ERR_INTERNAL, [player, target_pts_us, type]() {
        if (!is_valid_seek_type(type)) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "seek type out of range");
            return NAKI_VR_ERR_INVALID_ARGUMENT;
        }
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        auto seek_type = static_cast<vr::SeekType>(type);
        p->seek(target_pts_us, seek_type);
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

void naki_vr_player_set_speed(naki_vr_player_t player, double speed) noexcept {
    (void)naki_vr_player_set_speed_status(player, speed);
}

naki_vr_status_t naki_vr_player_set_speed_status(naki_vr_player_t player, double speed) noexcept {
    return ffi_guard("naki_vr_player_set_speed_status", NAKI_VR_ERR_INTERNAL, [player, speed]() {
        if (auto result = vr::validate_playback_speed(speed); !result.ok) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
            return NAKI_VR_ERR_INVALID_ARGUMENT;
        }
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        p->set_speed(speed);
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

void naki_vr_player_set_loop_range(naki_vr_player_t player,
                                   int enabled,
                                   int64_t start_us,
                                   int64_t end_us) noexcept {
    (void)naki_vr_player_set_loop_range_status(player, enabled, start_us, end_us);
}

naki_vr_status_t naki_vr_player_set_loop_range_status(naki_vr_player_t player,
                                                      int enabled,
                                                      int64_t start_us,
                                                      int64_t end_us) noexcept {
    return ffi_guard("naki_vr_player_set_loop_range_status", NAKI_VR_ERR_INTERNAL, [player, enabled, start_us, end_us]() {
        if (auto result = vr::validate_loop_range(enabled != 0, start_us, end_us); !result.ok) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
            return NAKI_VR_ERR_INVALID_ARGUMENT;
        }
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        p->set_loop_range(enabled != 0, start_us, end_us);
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

void naki_vr_player_set_audible_track(naki_vr_player_t player, int file_id) noexcept {
    (void)naki_vr_player_set_audible_track_status(player, file_id);
}

naki_vr_status_t naki_vr_player_set_audible_track_status(naki_vr_player_t player, int file_id) noexcept {
    return ffi_guard("naki_vr_player_set_audible_track_status", NAKI_VR_ERR_INTERNAL, [player, file_id]() {
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        p->set_audible_track(file_id);
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

/* ---- Frame stepping ---- */

void naki_vr_player_step_forward(naki_vr_player_t player) noexcept {
    (void)naki_vr_player_step_forward_status(player);
}

naki_vr_status_t naki_vr_player_step_forward_status(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_step_forward_status", NAKI_VR_ERR_INTERNAL, [player]() {
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        p->step_forward();
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

void naki_vr_player_step_backward(naki_vr_player_t player) noexcept {
    (void)naki_vr_player_step_backward_status(player);
}

naki_vr_status_t naki_vr_player_step_backward_status(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_step_backward_status", NAKI_VR_ERR_INTERNAL, [player]() {
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        p->step_backward();
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

/* ---- Query ---- */

int naki_vr_player_is_playing(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_is_playing", 0, [player]() {
        auto p = checked_player(player);
        return p && p->is_playing() ? 1 : 0;
    });
}

int naki_vr_player_is_initialized(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_is_initialized", 0, [player]() {
        auto p = checked_player(player);
        return p && p->is_initialized() ? 1 : 0;
    });
}

int64_t naki_vr_player_current_pts_us(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_current_pts_us", int64_t(0), [player]() {
        auto p = checked_player(player);
        return p ? p->current_pts_us() : int64_t(0);
    });
}

double naki_vr_player_current_speed(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_current_speed", 1.0, [player]() {
        auto p = checked_player(player);
        return p ? p->current_speed() : 1.0;
    });
}

int naki_vr_player_track_count(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_track_count", 0, [player]() {
        auto p = checked_player(player);
        return p ? static_cast<int>(p->track_count()) : 0;
    });
}

int64_t naki_vr_player_duration_us(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_duration_us", int64_t(0), [player]() {
        auto p = checked_player(player);
        return p ? p->duration_us() : int64_t(0);
    });
}

/* ---- Dynamic track management ---- */

int naki_vr_player_add_track(naki_vr_player_t player, const char* video_path) noexcept {
    return ffi_guard("naki_vr_player_add_track", -1, [player, video_path]() {
        int slot = -1;
        return naki_vr_player_add_track_status(player, video_path, &slot) == NAKI_VR_OK
            ? slot
            : -1;
    });
}

naki_vr_status_t naki_vr_player_add_track_status(naki_vr_player_t player,
                                                 const char* video_path,
                                                 int* out_slot) noexcept {
    return ffi_guard("naki_vr_player_add_track_status", NAKI_VR_ERR_INTERNAL, [player, video_path, out_slot]() {
        if (out_slot) {
            *out_slot = -1;
        }
        if (!video_path) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "video_path is required");
            return NAKI_VR_ERR_INVALID_ARGUMENT;
        }
        if (auto result = vr::validate_renderer_video_path(video_path); !result.ok) {
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
    });
}

void naki_vr_player_remove_track(naki_vr_player_t player, int file_id) noexcept {
    (void)naki_vr_player_remove_track_status(player, file_id);
}

naki_vr_status_t naki_vr_player_remove_track_status(naki_vr_player_t player, int file_id) noexcept {
    return ffi_guard("naki_vr_player_remove_track_status", NAKI_VR_ERR_INTERNAL, [player, file_id]() {
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        p->remove_track(file_id);
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

int naki_vr_player_has_track(naki_vr_player_t player, int slot) noexcept {
    return ffi_guard("naki_vr_player_has_track", 0, [player, slot]() {
        auto p = checked_player(player);
        return p && p->has_track(slot) ? 1 : 0;
    });
}

void naki_vr_player_set_track_offset(naki_vr_player_t player, int file_id, int64_t offset_us) noexcept {
    (void)naki_vr_player_set_track_offset_status(player, file_id, offset_us);
}

naki_vr_status_t naki_vr_player_set_track_offset_status(naki_vr_player_t player, int file_id, int64_t offset_us) noexcept {
    return ffi_guard("naki_vr_player_set_track_offset_status", NAKI_VR_ERR_INTERNAL, [player, file_id, offset_us]() {
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        p->set_track_offset(file_id, offset_us);
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

/* ---- Layout ---- */

void naki_vr_player_apply_layout(naki_vr_player_t player, const naki_vr_player_layout_state_t* state) noexcept {
    (void)naki_vr_player_apply_layout_status(player, state);
}

naki_vr_status_t naki_vr_player_apply_layout_status(naki_vr_player_t player, const naki_vr_player_layout_state_t* state) noexcept {
    return ffi_guard("naki_vr_player_apply_layout_status", NAKI_VR_ERR_INTERNAL, [player, state]() {
        if (!state) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "layout state is required");
            return NAKI_VR_ERR_INVALID_ARGUMENT;
        }
        if (!validate_ffi_layout_state(*state)) {
            return g_last_error.status;
        }
        auto p = checked_player(player);
        if (!p) return g_last_error.status;
        vr::LayoutState layout = to_layout_state(*state);
        p->apply_layout(layout);
        set_lease_ok(p);
        return NAKI_VR_OK;
    });
}

void naki_vr_player_layout(naki_vr_player_t player, naki_vr_player_layout_state_t* out_state) noexcept {
    ffi_guard_void("naki_vr_player_layout", [player, out_state]() {
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
        auto layout = p->layout();
        out_state->size = sizeof(naki_vr_player_layout_state_t);
        out_state->abi_version = NAKI_VR_ABI_VERSION;
        out_state->mode = layout.mode;
        out_state->split_pos = layout.split_pos;
        out_state->zoom_ratio = layout.zoom_ratio;
        out_state->view_offset[0] = layout.view_offset[0];
        out_state->view_offset[1] = layout.view_offset[1];
        out_state->pixel_size_mode = layout.pixel_size_mode;
        for (int i = 0; i < 4; ++i) out_state->order[i] = layout.order[i];
        set_ok();
    });
}

/* ---- Logging & Crash ---- */

void naki_vr_configure_logging(const naki_vr_log_config_t* config) noexcept {
    (void)naki_vr_configure_logging_status(config);
}

naki_vr_status_t naki_vr_configure_logging_status(const naki_vr_log_config_t* config) noexcept {
    return ffi_guard("naki_vr_configure_logging_status", NAKI_VR_ERR_INTERNAL, [config]() {
        if (!config) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "log config is required");
            return NAKI_VR_ERR_INVALID_ARGUMENT;
        }
        vr::LogConfig cfg;
        if (!to_log_config(*config, cfg)) {
            return g_last_error.status;
        }
        vr::configure_logging(cfg);
        set_ok();
        return NAKI_VR_OK;
    });
}

void naki_vr_install_crash_handler(const char* crash_dir) noexcept {
    (void)naki_vr_install_crash_handler_status(crash_dir);
}

naki_vr_status_t naki_vr_install_crash_handler_status(const char* crash_dir) noexcept {
    return ffi_guard("naki_vr_install_crash_handler_status", NAKI_VR_ERR_INTERNAL, [crash_dir]() {
        vr::install_crash_handler(crash_dir ? crash_dir : "");
        set_ok();
        return NAKI_VR_OK;
    });
}

void naki_vr_remove_crash_handler(void) noexcept {
    (void)naki_vr_remove_crash_handler_status();
}

naki_vr_status_t naki_vr_remove_crash_handler_status(void) noexcept {
    return ffi_guard("naki_vr_remove_crash_handler_status", NAKI_VR_ERR_INTERNAL, []() {
        vr::remove_crash_handler();
        set_ok();
        return NAKI_VR_OK;
    });
}
