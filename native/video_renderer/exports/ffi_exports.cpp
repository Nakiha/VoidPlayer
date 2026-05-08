#include "video_renderer/exports/ffi_exports.h"
#include "player/native_player.h"
#include "common/logging.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct FfiError {
    naki_vr_status_t status = NAKI_VR_OK;
    std::string message;
};

thread_local FfiError g_last_error;
std::mutex g_players_mutex;
std::unordered_set<vr::NativePlayer*> g_live_players;

vr::NativePlayer* raw_player(naki_vr_player_t player) {
    return static_cast<vr::NativePlayer*>(player);
}

void set_error(naki_vr_status_t status, std::string message) {
    g_last_error.status = status;
    g_last_error.message = std::move(message);
}

void set_ok() {
    set_error(NAKI_VR_OK, "");
}

void register_player(vr::NativePlayer* player) {
    std::lock_guard<std::mutex> lock(g_players_mutex);
    g_live_players.insert(player);
}

bool unregister_player(vr::NativePlayer* player) {
    std::lock_guard<std::mutex> lock(g_players_mutex);
    return g_live_players.erase(player) > 0;
}

vr::NativePlayer* checked_player(naki_vr_player_t player) {
    if (!player) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "player is required");
        return nullptr;
    }
    auto* typed = raw_player(player);
    std::lock_guard<std::mutex> lock(g_players_mutex);
    if (g_live_players.find(typed) == g_live_players.end()) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "player handle is invalid or destroyed");
        return nullptr;
    }
    return typed;
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
    if (c.width <= 0 || c.height <= 0) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "player dimensions must be positive");
        return false;
    }
    return validate_log_config(c.log_config);
}

bool is_valid_seek_type(int type) {
    return type == NAKI_VR_SEEK_KEYFRAME || type == NAKI_VR_SEEK_EXACT;
}

bool validate_layout_state(const naki_vr_player_layout_state_t& state) {
    if (!validate_abi(state.size,
                      state.abi_version,
                      sizeof(naki_vr_player_layout_state_t),
                      "layout state")) {
        return false;
    }
    if (state.mode != NAKI_VR_LAYOUT_SIDE_BY_SIDE &&
        state.mode != NAKI_VR_LAYOUT_SPLIT_SCREEN) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "layout mode out of range");
        return false;
    }
    if (state.pixel_size_mode != NAKI_VR_PIXEL_SIZE_UNIFORM_VIDEO_PIXELS &&
        state.pixel_size_mode != NAKI_VR_PIXEL_SIZE_FILL_VIEW) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "pixel size mode out of range");
        return false;
    }
    if (!std::isfinite(state.split_pos) ||
        !std::isfinite(state.zoom_ratio) ||
        !std::isfinite(state.view_offset[0]) ||
        !std::isfinite(state.view_offset[1]) ||
        state.zoom_ratio <= 0.0f) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "layout values must be finite and positive");
        return false;
    }
    return true;
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
    if (buf && cap > 0) {
        const auto& message = g_last_error.message;
        const size_t to_copy = std::min(cap - 1, message.size());
        std::memcpy(buf, message.data(), to_copy);
        buf[to_copy] = '\0';
    }
    return g_last_error.status;
}

naki_vr_player_t naki_vr_player_create(void) noexcept {
    return ffi_guard("naki_vr_player_create", static_cast<naki_vr_player_t>(nullptr), []() {
        auto* player = new vr::NativePlayer();
        register_player(player);
        set_ok();
        return static_cast<naki_vr_player_t>(player);
    });
}

void naki_vr_player_destroy(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_destroy", [player]() {
        if (!player) {
            set_ok();
            return;
        }
        auto* typed = raw_player(player);
        if (!unregister_player(typed)) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "player handle is invalid or destroyed");
            return;
        }
        delete typed;
        set_ok();
    });
}

int naki_vr_player_initialize(naki_vr_player_t player, const naki_vr_player_config_t* config) noexcept {
    return ffi_guard("naki_vr_player_initialize", 0, [player, config]() {
        if (!config) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "config is required");
            return 0;
        }
        if (!validate_player_config(*config)) {
            return 0;
        }
        auto* p = checked_player(player);
        if (!p) return 0;

        vr::RendererConfig cfg;
        cfg.hwnd = reinterpret_cast<void*>(config->hwnd);
        cfg.width = config->width;
        cfg.height = config->height;
        cfg.use_hardware_decode = config->use_hardware_decode != 0;
        if (!to_log_config(config->log_config, cfg.log_config)) {
            return 0;
        }

        if (config->video_paths) {
            for (auto path = config->video_paths; *path; ++path) {
                if (!*path) {
                    set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "video path must not be null");
                    return 0;
                }
                cfg.video_paths.emplace_back(*path);
            }
        }

        if (!p->initialize(cfg)) {
            set_error(NAKI_VR_ERR_OPEN_FAILED, "player initialize failed");
            return 0;
        }
        set_ok();
        return 1;
    });
}

void naki_vr_player_shutdown(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_shutdown", [player]() {
        auto* p = checked_player(player);
        if (!p) return;
        p->shutdown();
        set_ok();
    });
}

/* ---- Playback ---- */

void naki_vr_player_play(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_play", [player]() {
        auto* p = checked_player(player);
        if (!p) return;
        p->play();
        set_ok();
    });
}

void naki_vr_player_pause(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_pause", [player]() {
        auto* p = checked_player(player);
        if (!p) return;
        p->pause();
        set_ok();
    });
}

void naki_vr_player_seek(naki_vr_player_t player, int64_t target_pts_us) noexcept {
    ffi_guard_void("naki_vr_player_seek", [player, target_pts_us]() {
        auto* p = checked_player(player);
        if (!p) return;
        p->seek(target_pts_us);
        set_ok();
    });
}

void naki_vr_player_seek_typed(naki_vr_player_t player, int64_t target_pts_us, int type) noexcept {
    ffi_guard_void("naki_vr_player_seek_typed", [player, target_pts_us, type]() {
        if (!is_valid_seek_type(type)) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "seek type out of range");
            return;
        }
        auto* p = checked_player(player);
        if (!p) return;
        auto seek_type = static_cast<vr::SeekType>(type);
        p->seek(target_pts_us, seek_type);
        set_ok();
    });
}

void naki_vr_player_set_speed(naki_vr_player_t player, double speed) noexcept {
    ffi_guard_void("naki_vr_player_set_speed", [player, speed]() {
        if (!std::isfinite(speed) || speed <= 0.0 || speed > 16.0) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "speed out of range");
            return;
        }
        auto* p = checked_player(player);
        if (!p) return;
        p->set_speed(speed);
        set_ok();
    });
}

void naki_vr_player_set_loop_range(naki_vr_player_t player,
                                   int enabled,
                                   int64_t start_us,
                                   int64_t end_us) noexcept {
    ffi_guard_void("naki_vr_player_set_loop_range", [player, enabled, start_us, end_us]() {
        auto* p = checked_player(player);
        if (!p) return;
        p->set_loop_range(enabled != 0, start_us, end_us);
        set_ok();
    });
}

void naki_vr_player_set_audible_track(naki_vr_player_t player, int file_id) noexcept {
    ffi_guard_void("naki_vr_player_set_audible_track", [player, file_id]() {
        auto* p = checked_player(player);
        if (!p) return;
        p->set_audible_track(file_id);
        set_ok();
    });
}

/* ---- Frame stepping ---- */

void naki_vr_player_step_forward(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_step_forward", [player]() {
        auto* p = checked_player(player);
        if (!p) return;
        p->step_forward();
        set_ok();
    });
}

void naki_vr_player_step_backward(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_step_backward", [player]() {
        auto* p = checked_player(player);
        if (!p) return;
        p->step_backward();
        set_ok();
    });
}

/* ---- Query ---- */

int naki_vr_player_is_playing(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_is_playing", 0, [player]() {
        auto* p = checked_player(player);
        return p && p->is_playing() ? 1 : 0;
    });
}

int naki_vr_player_is_initialized(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_is_initialized", 0, [player]() {
        auto* p = checked_player(player);
        return p && p->is_initialized() ? 1 : 0;
    });
}

int64_t naki_vr_player_current_pts_us(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_current_pts_us", int64_t(0), [player]() {
        auto* p = checked_player(player);
        return p ? p->current_pts_us() : int64_t(0);
    });
}

double naki_vr_player_current_speed(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_current_speed", 1.0, [player]() {
        auto* p = checked_player(player);
        return p ? p->current_speed() : 1.0;
    });
}

int naki_vr_player_track_count(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_track_count", 0, [player]() {
        auto* p = checked_player(player);
        return p ? static_cast<int>(p->track_count()) : 0;
    });
}

int64_t naki_vr_player_duration_us(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_duration_us", int64_t(0), [player]() {
        auto* p = checked_player(player);
        return p ? p->duration_us() : int64_t(0);
    });
}

/* ---- Dynamic track management ---- */

int naki_vr_player_add_track(naki_vr_player_t player, const char* video_path) noexcept {
    return ffi_guard("naki_vr_player_add_track", -1, [player, video_path]() {
        if (!video_path) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "video_path is required");
            return -1;
        }
        auto* p = checked_player(player);
        if (!p) return -1;
        int slot = p->add_track(video_path);
        if (slot < 0) {
            set_error(NAKI_VR_ERR_OPEN_FAILED, "add_track failed");
            return -1;
        }
        set_ok();
        return slot;
    });
}

void naki_vr_player_remove_track(naki_vr_player_t player, int file_id) noexcept {
    ffi_guard_void("naki_vr_player_remove_track", [player, file_id]() {
        auto* p = checked_player(player);
        if (!p) return;
        p->remove_track(file_id);
        set_ok();
    });
}

int naki_vr_player_has_track(naki_vr_player_t player, int slot) noexcept {
    return ffi_guard("naki_vr_player_has_track", 0, [player, slot]() {
        auto* p = checked_player(player);
        return p && p->has_track(slot) ? 1 : 0;
    });
}

void naki_vr_player_set_track_offset(naki_vr_player_t player, int file_id, int64_t offset_us) noexcept {
    ffi_guard_void("naki_vr_player_set_track_offset", [player, file_id, offset_us]() {
        auto* p = checked_player(player);
        if (!p) return;
        p->set_track_offset(file_id, offset_us);
        set_ok();
    });
}

/* ---- Layout ---- */

void naki_vr_player_apply_layout(naki_vr_player_t player, const naki_vr_player_layout_state_t* state) noexcept {
    ffi_guard_void("naki_vr_player_apply_layout", [player, state]() {
        if (!state) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "layout state is required");
            return;
        }
        if (!validate_layout_state(*state)) {
            return;
        }
        auto* p = checked_player(player);
        if (!p) return;
        vr::LayoutState layout;
        layout.mode = state->mode;
        layout.split_pos = state->split_pos;
        layout.zoom_ratio = state->zoom_ratio;
        layout.view_offset[0] = state->view_offset[0];
        layout.view_offset[1] = state->view_offset[1];
        layout.pixel_size_mode = state->pixel_size_mode;
        for (int i = 0; i < 4; ++i) layout.order[i] = state->order[i];
        p->apply_layout(layout);
        set_ok();
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
        auto* p = checked_player(player);
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
    ffi_guard_void("naki_vr_configure_logging", [config]() {
        if (!config) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "log config is required");
            return;
        }
        vr::LogConfig cfg;
        if (!to_log_config(*config, cfg)) {
            return;
        }
        vr::configure_logging(cfg);
        set_ok();
    });
}

void naki_vr_install_crash_handler(const char* crash_dir) noexcept {
    ffi_guard_void("naki_vr_install_crash_handler", [crash_dir]() {
        vr::install_crash_handler(crash_dir ? crash_dir : "");
        set_ok();
    });
}

void naki_vr_remove_crash_handler(void) noexcept {
    ffi_guard_void("naki_vr_remove_crash_handler", []() {
        vr::remove_crash_handler();
        set_ok();
    });
}
