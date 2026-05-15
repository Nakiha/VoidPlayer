#include "video_renderer/exports/ffi_exports.h"
#include "video_renderer/exports/ffi_marshalling.h"
#include "video_renderer/exports/ffi_player_commands.h"
#include "video_renderer/exports/ffi_player_lifecycle.h"
#include "video_renderer/exports/ffi_player_registry.h"
#include "video_renderer/exports/ffi_process_globals.h"
#include <spdlog/spdlog.h>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vr::ffi;

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

naki_vr_status_t naki_vr_last_error(naki_vr_player_t player, char* buf, size_t cap) noexcept {
    return copy_global_error_lifecycle_command(player, buf, cap);
}

naki_vr_status_t naki_vr_player_get_error(naki_vr_player_t player, char* buf, size_t cap) noexcept {
    return ffi_guard("naki_vr_player_get_error", NAKI_VR_ERR_INTERNAL, [player, buf, cap]() {
        return copy_player_error_lifecycle_command(player, buf, cap);
    });
}

naki_vr_player_t naki_vr_player_create(void) noexcept {
    return ffi_guard("naki_vr_player_create", static_cast<naki_vr_player_t>(nullptr), []() {
        return create_player_lifecycle_command();
    });
}

naki_vr_status_t naki_vr_player_destroy_status(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_destroy_status", NAKI_VR_ERR_INTERNAL, [player]() {
        return destroy_player_lifecycle_command(player);
    });
}

void naki_vr_player_destroy(naki_vr_player_t player) noexcept {
    (void)naki_vr_player_destroy_status(player);
}

naki_vr_status_t naki_vr_player_initialize_v2(naki_vr_player_t player, const naki_vr_player_config_v2_t* config) noexcept {
    return ffi_guard("naki_vr_player_initialize_v2", NAKI_VR_ERR_INTERNAL, [player, config]() {
        return initialize_player_v2_lifecycle_command(player, config);
    });
}

naki_vr_status_t naki_vr_player_shutdown_status(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_shutdown_status", NAKI_VR_ERR_INTERNAL, [player]() {
        return shutdown_player_lifecycle_command(player);
    });
}

int naki_vr_player_initialize(naki_vr_player_t player, const naki_vr_player_config_t* config) noexcept {
    return ffi_guard("naki_vr_player_initialize", 0, [player, config]() {
        return initialize_player_v1_lifecycle_command(player, config);
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
        return play_player_command(player);
    });
}

void naki_vr_player_pause(naki_vr_player_t player) noexcept {
    (void)naki_vr_player_pause_status(player);
}

naki_vr_status_t naki_vr_player_pause_status(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_pause_status", NAKI_VR_ERR_INTERNAL, [player]() {
        return pause_player_command(player);
    });
}

void naki_vr_player_seek(naki_vr_player_t player, int64_t target_pts_us) noexcept {
    (void)naki_vr_player_seek_status(player, target_pts_us);
}

naki_vr_status_t naki_vr_player_seek_status(naki_vr_player_t player, int64_t target_pts_us) noexcept {
    return ffi_guard("naki_vr_player_seek_status", NAKI_VR_ERR_INTERNAL, [player, target_pts_us]() {
        return seek_player_command(player, target_pts_us);
    });
}

void naki_vr_player_seek_typed(naki_vr_player_t player, int64_t target_pts_us, int type) noexcept {
    (void)naki_vr_player_seek_typed_status(player, target_pts_us, type);
}

naki_vr_status_t naki_vr_player_seek_typed_status(naki_vr_player_t player, int64_t target_pts_us, int type) noexcept {
    return ffi_guard("naki_vr_player_seek_typed_status", NAKI_VR_ERR_INTERNAL, [player, target_pts_us, type]() {
        return seek_typed_player_command(player, target_pts_us, type);
    });
}

void naki_vr_player_set_speed(naki_vr_player_t player, double speed) noexcept {
    (void)naki_vr_player_set_speed_status(player, speed);
}

naki_vr_status_t naki_vr_player_set_speed_status(naki_vr_player_t player, double speed) noexcept {
    return ffi_guard("naki_vr_player_set_speed_status", NAKI_VR_ERR_INTERNAL, [player, speed]() {
        return set_player_speed_command(player, speed);
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
        return set_player_loop_range_command(player, enabled, start_us, end_us);
    });
}

void naki_vr_player_set_audible_track(naki_vr_player_t player, int file_id) noexcept {
    (void)naki_vr_player_set_audible_track_status(player, file_id);
}

naki_vr_status_t naki_vr_player_set_audible_track_status(naki_vr_player_t player, int file_id) noexcept {
    return ffi_guard("naki_vr_player_set_audible_track_status", NAKI_VR_ERR_INTERNAL, [player, file_id]() {
        return set_player_audible_track_command(player, file_id);
    });
}

/* ---- Frame stepping ---- */

void naki_vr_player_step_forward(naki_vr_player_t player) noexcept {
    (void)naki_vr_player_step_forward_status(player);
}

naki_vr_status_t naki_vr_player_step_forward_status(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_step_forward_status", NAKI_VR_ERR_INTERNAL, [player]() {
        return step_player_forward_command(player);
    });
}

void naki_vr_player_step_backward(naki_vr_player_t player) noexcept {
    (void)naki_vr_player_step_backward_status(player);
}

naki_vr_status_t naki_vr_player_step_backward_status(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_step_backward_status", NAKI_VR_ERR_INTERNAL, [player]() {
        return step_player_backward_command(player);
    });
}

/* ---- Query ---- */

int naki_vr_player_is_playing(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_is_playing", 0, [player]() {
        return query_player_is_playing(player);
    });
}

int naki_vr_player_is_initialized(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_is_initialized", 0, [player]() {
        return query_player_is_initialized(player);
    });
}

int64_t naki_vr_player_current_pts_us(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_current_pts_us", int64_t(0), [player]() {
        return query_player_current_pts_us(player);
    });
}

double naki_vr_player_current_speed(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_current_speed", 1.0, [player]() {
        return query_player_current_speed(player);
    });
}

int naki_vr_player_track_count(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_track_count", 0, [player]() {
        return query_player_track_count(player);
    });
}

int64_t naki_vr_player_duration_us(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_duration_us", int64_t(0), [player]() {
        return query_player_duration_us(player);
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
        return add_player_track_command(player, video_path, out_slot);
    });
}

void naki_vr_player_remove_track(naki_vr_player_t player, int file_id) noexcept {
    (void)naki_vr_player_remove_track_status(player, file_id);
}

naki_vr_status_t naki_vr_player_remove_track_status(naki_vr_player_t player, int file_id) noexcept {
    return ffi_guard("naki_vr_player_remove_track_status", NAKI_VR_ERR_INTERNAL, [player, file_id]() {
        return remove_player_track_command(player, file_id);
    });
}

int naki_vr_player_has_track(naki_vr_player_t player, int slot) noexcept {
    return ffi_guard("naki_vr_player_has_track", 0, [player, slot]() {
        return query_player_has_track(player, slot);
    });
}

void naki_vr_player_set_track_offset(naki_vr_player_t player, int file_id, int64_t offset_us) noexcept {
    (void)naki_vr_player_set_track_offset_status(player, file_id, offset_us);
}

naki_vr_status_t naki_vr_player_set_track_offset_status(naki_vr_player_t player, int file_id, int64_t offset_us) noexcept {
    return ffi_guard("naki_vr_player_set_track_offset_status", NAKI_VR_ERR_INTERNAL, [player, file_id, offset_us]() {
        return set_player_track_offset_command(player, file_id, offset_us);
    });
}

/* ---- Layout ---- */

void naki_vr_player_apply_layout(naki_vr_player_t player, const naki_vr_player_layout_state_t* state) noexcept {
    (void)naki_vr_player_apply_layout_status(player, state);
}

naki_vr_status_t naki_vr_player_apply_layout_status(naki_vr_player_t player, const naki_vr_player_layout_state_t* state) noexcept {
    return ffi_guard("naki_vr_player_apply_layout_status", NAKI_VR_ERR_INTERNAL, [player, state]() {
        return apply_player_layout_command(player, state);
    });
}

void naki_vr_player_layout(naki_vr_player_t player, naki_vr_player_layout_state_t* out_state) noexcept {
    ffi_guard_void("naki_vr_player_layout", [player, out_state]() {
        query_player_layout_command(player, out_state);
    });
}

/* ---- Logging & Crash ---- */

void naki_vr_configure_logging(const naki_vr_log_config_t* config) noexcept {
    (void)naki_vr_configure_logging_status(config);
}

naki_vr_status_t naki_vr_configure_logging_status(const naki_vr_log_config_t* config) noexcept {
    return ffi_guard("naki_vr_configure_logging_status", NAKI_VR_ERR_INTERNAL, [config]() {
        return configure_logging_process_command(config);
    });
}

void naki_vr_install_crash_handler(const char* crash_dir) noexcept {
    (void)naki_vr_install_crash_handler_status(crash_dir);
}

naki_vr_status_t naki_vr_install_crash_handler_status(const char* crash_dir) noexcept {
    return ffi_guard("naki_vr_install_crash_handler_status", NAKI_VR_ERR_INTERNAL, [crash_dir]() {
        return install_crash_handler_process_command(crash_dir);
    });
}

void naki_vr_remove_crash_handler(void) noexcept {
    (void)naki_vr_remove_crash_handler_status();
}

naki_vr_status_t naki_vr_remove_crash_handler_status(void) noexcept {
    return ffi_guard("naki_vr_remove_crash_handler_status", NAKI_VR_ERR_INTERNAL, []() {
        return remove_crash_handler_process_command();
    });
}
