#include "video_renderer/exports/ffi_exports.h"
#include "video_renderer/renderer.h"
#include "common/logging.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <exception>
#include <vector>
#include <string>
#include <utility>

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

naki_vr_renderer_t naki_vr_renderer_create(void) noexcept {
    return ffi_guard("naki_vr_renderer_create", static_cast<naki_vr_renderer_t>(nullptr), []() {
        return static_cast<naki_vr_renderer_t>(new vr::Renderer());
    });
}

void naki_vr_renderer_destroy(naki_vr_renderer_t renderer) noexcept {
    ffi_guard_void("naki_vr_renderer_destroy", [renderer]() {
        delete static_cast<vr::Renderer*>(renderer);
    });
}

int naki_vr_renderer_initialize(naki_vr_renderer_t renderer, const naki_vr_renderer_config_t* config) noexcept {
    return ffi_guard("naki_vr_renderer_initialize", 0, [renderer, config]() {
        if (!renderer || !config) return 0;
        auto* r = static_cast<vr::Renderer*>(renderer);

        vr::RendererConfig cfg;
        cfg.hwnd = reinterpret_cast<void*>(config->hwnd);
        cfg.width = config->width;
        cfg.height = config->height;
        cfg.use_hardware_decode = config->use_hardware_decode != 0;
        cfg.log_config = to_log_config(config->log_config);

        if (config->video_paths) {
            for (auto p = config->video_paths; *p; ++p) {
                cfg.video_paths.emplace_back(*p);
            }
        }

        return r->initialize(cfg) ? 1 : 0;
    });
}

void naki_vr_renderer_shutdown(naki_vr_renderer_t renderer) noexcept {
    ffi_guard_void("naki_vr_renderer_shutdown", [renderer]() {
        if (renderer) static_cast<vr::Renderer*>(renderer)->shutdown();
    });
}

/* ---- Playback ---- */

void naki_vr_renderer_play(naki_vr_renderer_t renderer) noexcept {
    ffi_guard_void("naki_vr_renderer_play", [renderer]() {
        if (renderer) static_cast<vr::Renderer*>(renderer)->play();
    });
}

void naki_vr_renderer_pause(naki_vr_renderer_t renderer) noexcept {
    ffi_guard_void("naki_vr_renderer_pause", [renderer]() {
        if (renderer) static_cast<vr::Renderer*>(renderer)->pause();
    });
}

void naki_vr_renderer_seek(naki_vr_renderer_t renderer, int64_t target_pts_us) noexcept {
    ffi_guard_void("naki_vr_renderer_seek", [renderer, target_pts_us]() {
        if (renderer) static_cast<vr::Renderer*>(renderer)->seek(target_pts_us);
    });
}

void naki_vr_renderer_seek_typed(naki_vr_renderer_t renderer, int64_t target_pts_us, int type) noexcept {
    ffi_guard_void("naki_vr_renderer_seek_typed", [renderer, target_pts_us, type]() {
        if (renderer) {
            auto seek_type = static_cast<vr::SeekType>(type);
            static_cast<vr::Renderer*>(renderer)->seek(target_pts_us, seek_type);
        }
    });
}

void naki_vr_renderer_set_speed(naki_vr_renderer_t renderer, double speed) noexcept {
    ffi_guard_void("naki_vr_renderer_set_speed", [renderer, speed]() {
        if (renderer) static_cast<vr::Renderer*>(renderer)->set_speed(speed);
    });
}

/* ---- Frame stepping ---- */

void naki_vr_renderer_step_forward(naki_vr_renderer_t renderer) noexcept {
    ffi_guard_void("naki_vr_renderer_step_forward", [renderer]() {
        if (renderer) static_cast<vr::Renderer*>(renderer)->step_forward();
    });
}

void naki_vr_renderer_step_backward(naki_vr_renderer_t renderer) noexcept {
    ffi_guard_void("naki_vr_renderer_step_backward", [renderer]() {
        if (renderer) static_cast<vr::Renderer*>(renderer)->step_backward();
    });
}

/* ---- Query ---- */

int naki_vr_renderer_is_playing(naki_vr_renderer_t renderer) noexcept {
    return ffi_guard("naki_vr_renderer_is_playing", 0, [renderer]() {
        return renderer && static_cast<vr::Renderer*>(renderer)->is_playing() ? 1 : 0;
    });
}

int naki_vr_renderer_is_initialized(naki_vr_renderer_t renderer) noexcept {
    return ffi_guard("naki_vr_renderer_is_initialized", 0, [renderer]() {
        return renderer && static_cast<vr::Renderer*>(renderer)->is_initialized() ? 1 : 0;
    });
}

int64_t naki_vr_renderer_current_pts_us(naki_vr_renderer_t renderer) noexcept {
    return ffi_guard("naki_vr_renderer_current_pts_us", int64_t(0), [renderer]() {
        return renderer ? static_cast<vr::Renderer*>(renderer)->current_pts_us() : int64_t(0);
    });
}

double naki_vr_renderer_current_speed(naki_vr_renderer_t renderer) noexcept {
    return ffi_guard("naki_vr_renderer_current_speed", 1.0, [renderer]() {
        return renderer ? static_cast<vr::Renderer*>(renderer)->current_speed() : 1.0;
    });
}

int naki_vr_renderer_track_count(naki_vr_renderer_t renderer) noexcept {
    return ffi_guard("naki_vr_renderer_track_count", 0, [renderer]() {
        return renderer ? static_cast<int>(static_cast<vr::Renderer*>(renderer)->track_count()) : 0;
    });
}

int64_t naki_vr_renderer_duration_us(naki_vr_renderer_t renderer) noexcept {
    return ffi_guard("naki_vr_renderer_duration_us", int64_t(0), [renderer]() {
        return renderer ? static_cast<vr::Renderer*>(renderer)->duration_us() : int64_t(0);
    });
}

/* ---- Layout ---- */

void naki_vr_renderer_apply_layout(naki_vr_renderer_t renderer, const naki_vr_layout_state_t* state) noexcept {
    ffi_guard_void("naki_vr_renderer_apply_layout", [renderer, state]() {
        if (!renderer || !state) return;
        vr::LayoutState ls;
        ls.mode = state->mode;
        ls.split_pos = state->split_pos;
        ls.zoom_ratio = state->zoom_ratio;
        ls.view_offset[0] = state->view_offset[0];
        ls.view_offset[1] = state->view_offset[1];
        for (int i = 0; i < 4; ++i) ls.order[i] = state->order[i];
        static_cast<vr::Renderer*>(renderer)->apply_layout(ls);
    });
}

void naki_vr_renderer_layout(naki_vr_renderer_t renderer, naki_vr_layout_state_t* out_state) noexcept {
    ffi_guard_void("naki_vr_renderer_layout", [renderer, out_state]() {
        if (!renderer || !out_state) return;
        auto ls = static_cast<vr::Renderer*>(renderer)->layout();
        out_state->mode = ls.mode;
        out_state->split_pos = ls.split_pos;
        out_state->zoom_ratio = ls.zoom_ratio;
        out_state->view_offset[0] = ls.view_offset[0];
        out_state->view_offset[1] = ls.view_offset[1];
        for (int i = 0; i < 4; ++i) out_state->order[i] = ls.order[i];
    });
}

/* ---- Dynamic track management ---- */

int naki_vr_renderer_add_track(naki_vr_renderer_t renderer, const char* video_path) noexcept {
    return ffi_guard("naki_vr_renderer_add_track", -1, [renderer, video_path]() {
        if (!renderer || !video_path) return -1;
        return static_cast<vr::Renderer*>(renderer)->add_track(video_path);
    });
}

void naki_vr_renderer_remove_track(naki_vr_renderer_t renderer, int file_id) noexcept {
    ffi_guard_void("naki_vr_renderer_remove_track", [renderer, file_id]() {
        if (renderer) static_cast<vr::Renderer*>(renderer)->remove_track(file_id);
    });
}

int naki_vr_renderer_has_track(naki_vr_renderer_t renderer, int slot) noexcept {
    return ffi_guard("naki_vr_renderer_has_track", 0, [renderer, slot]() {
        if (!renderer) return 0;
        return static_cast<vr::Renderer*>(renderer)->has_track(slot) ? 1 : 0;
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
