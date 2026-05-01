#include "video_renderer/exports/ffi_exports.h"
#include "player/native_player.h"
#include "common/logging.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

static vr::LogConfig to_log_config(const naki_vr_log_config_t& c) {
    vr::LogConfig cfg;
    cfg.pattern = c.pattern ? c.pattern : "";
    cfg.file_path = c.file_path ? c.file_path : "";
    cfg.max_file_size = c.max_file_size;
    cfg.max_files = c.max_files;
    cfg.level = static_cast<spdlog::level::level_enum>(c.level);
    return cfg;
}

namespace {

vr::NativePlayer* as_player(naki_vr_player_t player) {
    return static_cast<vr::NativePlayer*>(player);
}

template <typename Fn, typename T>
T ffi_guard(const char* name, T fallback, Fn&& fn) noexcept {
    try {
        return std::forward<Fn>(fn)();
    } catch (const std::exception& e) {
        spdlog::error("{} exception: {}", name, e.what());
        return fallback;
    } catch (...) {
        spdlog::error("{} unknown exception", name);
        return fallback;
    }
}

template <typename Fn>
void ffi_guard_void(const char* name, Fn&& fn) noexcept {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::exception& e) {
        spdlog::error("{} exception: {}", name, e.what());
    } catch (...) {
        spdlog::error("{} unknown exception", name);
    }
}

} // namespace

/* ---- Lifecycle ---- */

naki_vr_player_t naki_vr_player_create(void) noexcept {
    return ffi_guard("naki_vr_player_create", static_cast<naki_vr_player_t>(nullptr), []() {
        return static_cast<naki_vr_player_t>(new vr::NativePlayer());
    });
}

void naki_vr_player_destroy(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_destroy", [player]() {
        delete as_player(player);
    });
}

int naki_vr_player_initialize(naki_vr_player_t player, const naki_vr_player_config_t* config) noexcept {
    return ffi_guard("naki_vr_player_initialize", 0, [player, config]() {
        if (!player || !config) return 0;
        auto* p = as_player(player);

        vr::RendererConfig cfg;
        cfg.hwnd = reinterpret_cast<void*>(config->hwnd);
        cfg.width = config->width;
        cfg.height = config->height;
        cfg.use_hardware_decode = config->use_hardware_decode != 0;
        cfg.log_config = to_log_config(config->log_config);

        if (config->video_paths) {
            for (auto path = config->video_paths; *path; ++path) {
                cfg.video_paths.emplace_back(*path);
            }
        }

        return p->initialize(cfg) ? 1 : 0;
    });
}

void naki_vr_player_shutdown(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_shutdown", [player]() {
        if (player) as_player(player)->shutdown();
    });
}

/* ---- Playback ---- */

void naki_vr_player_play(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_play", [player]() {
        if (player) as_player(player)->play();
    });
}

void naki_vr_player_pause(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_pause", [player]() {
        if (player) as_player(player)->pause();
    });
}

void naki_vr_player_seek(naki_vr_player_t player, int64_t target_pts_us) noexcept {
    ffi_guard_void("naki_vr_player_seek", [player, target_pts_us]() {
        if (player) as_player(player)->seek(target_pts_us);
    });
}

void naki_vr_player_seek_typed(naki_vr_player_t player, int64_t target_pts_us, int type) noexcept {
    ffi_guard_void("naki_vr_player_seek_typed", [player, target_pts_us, type]() {
        if (player) {
            auto seek_type = static_cast<vr::SeekType>(type);
            as_player(player)->seek(target_pts_us, seek_type);
        }
    });
}

void naki_vr_player_set_speed(naki_vr_player_t player, double speed) noexcept {
    ffi_guard_void("naki_vr_player_set_speed", [player, speed]() {
        if (player) as_player(player)->set_speed(speed);
    });
}

void naki_vr_player_set_loop_range(naki_vr_player_t player,
                                   int enabled,
                                   int64_t start_us,
                                   int64_t end_us) noexcept {
    ffi_guard_void("naki_vr_player_set_loop_range", [player, enabled, start_us, end_us]() {
        if (player) as_player(player)->set_loop_range(enabled != 0, start_us, end_us);
    });
}

void naki_vr_player_set_audible_track(naki_vr_player_t player, int file_id) noexcept {
    ffi_guard_void("naki_vr_player_set_audible_track", [player, file_id]() {
        if (player) as_player(player)->set_audible_track(file_id);
    });
}

/* ---- Frame stepping ---- */

void naki_vr_player_step_forward(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_step_forward", [player]() {
        if (player) as_player(player)->step_forward();
    });
}

void naki_vr_player_step_backward(naki_vr_player_t player) noexcept {
    ffi_guard_void("naki_vr_player_step_backward", [player]() {
        if (player) as_player(player)->step_backward();
    });
}

/* ---- Query ---- */

int naki_vr_player_is_playing(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_is_playing", 0, [player]() {
        return player && as_player(player)->is_playing() ? 1 : 0;
    });
}

int naki_vr_player_is_initialized(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_is_initialized", 0, [player]() {
        return player && as_player(player)->is_initialized() ? 1 : 0;
    });
}

int64_t naki_vr_player_current_pts_us(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_current_pts_us", int64_t(0), [player]() {
        return player ? as_player(player)->current_pts_us() : int64_t(0);
    });
}

double naki_vr_player_current_speed(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_current_speed", 1.0, [player]() {
        return player ? as_player(player)->current_speed() : 1.0;
    });
}

int naki_vr_player_track_count(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_track_count", 0, [player]() {
        return player ? static_cast<int>(as_player(player)->track_count()) : 0;
    });
}

int64_t naki_vr_player_duration_us(naki_vr_player_t player) noexcept {
    return ffi_guard("naki_vr_player_duration_us", int64_t(0), [player]() {
        return player ? as_player(player)->duration_us() : int64_t(0);
    });
}

/* ---- Dynamic track management ---- */

int naki_vr_player_add_track(naki_vr_player_t player, const char* video_path) noexcept {
    return ffi_guard("naki_vr_player_add_track", -1, [player, video_path]() {
        if (!player || !video_path) return -1;
        return as_player(player)->add_track(video_path);
    });
}

void naki_vr_player_remove_track(naki_vr_player_t player, int file_id) noexcept {
    ffi_guard_void("naki_vr_player_remove_track", [player, file_id]() {
        if (player) as_player(player)->remove_track(file_id);
    });
}

int naki_vr_player_has_track(naki_vr_player_t player, int slot) noexcept {
    return ffi_guard("naki_vr_player_has_track", 0, [player, slot]() {
        if (!player) return 0;
        return as_player(player)->has_track(slot) ? 1 : 0;
    });
}

void naki_vr_player_set_track_offset(naki_vr_player_t player, int file_id, int64_t offset_us) noexcept {
    ffi_guard_void("naki_vr_player_set_track_offset", [player, file_id, offset_us]() {
        if (player) as_player(player)->set_track_offset(file_id, offset_us);
    });
}

/* ---- Layout ---- */

void naki_vr_player_apply_layout(naki_vr_player_t player, const naki_vr_player_layout_state_t* state) noexcept {
    ffi_guard_void("naki_vr_player_apply_layout", [player, state]() {
        if (!player || !state) return;
        vr::LayoutState layout;
        layout.mode = state->mode;
        layout.split_pos = state->split_pos;
        layout.zoom_ratio = state->zoom_ratio;
        layout.view_offset[0] = state->view_offset[0];
        layout.view_offset[1] = state->view_offset[1];
        for (int i = 0; i < 4; ++i) layout.order[i] = state->order[i];
        as_player(player)->apply_layout(layout);
    });
}

void naki_vr_player_layout(naki_vr_player_t player, naki_vr_player_layout_state_t* out_state) noexcept {
    ffi_guard_void("naki_vr_player_layout", [player, out_state]() {
        if (!player || !out_state) return;
        auto layout = as_player(player)->layout();
        out_state->mode = layout.mode;
        out_state->split_pos = layout.split_pos;
        out_state->zoom_ratio = layout.zoom_ratio;
        out_state->view_offset[0] = layout.view_offset[0];
        out_state->view_offset[1] = layout.view_offset[1];
        for (int i = 0; i < 4; ++i) out_state->order[i] = layout.order[i];
    });
}

/* ---- Logging & Crash ---- */

void naki_vr_configure_logging(const naki_vr_log_config_t* config) noexcept {
    ffi_guard_void("naki_vr_configure_logging", [config]() {
        if (!config) return;
        vr::configure_logging(to_log_config(*config));
    });
}

void naki_vr_install_crash_handler(const char* crash_dir) noexcept {
    ffi_guard_void("naki_vr_install_crash_handler", [crash_dir]() {
        vr::install_crash_handler(crash_dir ? crash_dir : "");
    });
}

void naki_vr_remove_crash_handler(void) noexcept {
    ffi_guard_void("naki_vr_remove_crash_handler", []() {
        vr::remove_crash_handler();
    });
}
