#include "video_renderer/exports/ffi_exports.h"
#include "video_renderer/exports/ffi_marshalling.h"
#include "video_renderer/exports/ffi_player_registry.h"
#include "video_renderer/renderer_config_validation.h"
#include "common/logging.h"
#include "common/windows_crash_handler.h"
#include <spdlog/spdlog.h>
#include <exception>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vr::ffi;

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
        p->seek(target_pts_us, to_seek_type(type));
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
        fill_ffi_layout_state(layout, *out_state);
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
