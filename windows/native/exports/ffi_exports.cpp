#include "exports/ffi_exports.h"
#include "video_renderer/renderer.h"
#include "video_renderer/logging.h"
#include <cstring>
#include <vector>
#include <string>

static vr::LogConfig to_log_config(const naki_vr_log_config_t& c) {
    vr::LogConfig cfg;
    cfg.pattern = c.pattern ? c.pattern : "";
    cfg.file_path = c.file_path ? c.file_path : "";
    cfg.max_file_size = c.max_file_size;
    cfg.max_files = c.max_files;
    cfg.level = static_cast<spdlog::level::level_enum>(c.level);
    return cfg;
}

/* ---- Lifecycle ---- */

naki_vr_renderer_t naki_vr_renderer_create(void) {
    return new vr::Renderer();
}

void naki_vr_renderer_destroy(naki_vr_renderer_t renderer) {
    delete static_cast<vr::Renderer*>(renderer);
}

int naki_vr_renderer_initialize(naki_vr_renderer_t renderer, const naki_vr_renderer_config_t* config) {
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
}

void naki_vr_renderer_shutdown(naki_vr_renderer_t renderer) {
    if (renderer) static_cast<vr::Renderer*>(renderer)->shutdown();
}

/* ---- Playback ---- */

void naki_vr_renderer_play(naki_vr_renderer_t renderer) {
    if (renderer) static_cast<vr::Renderer*>(renderer)->play();
}

void naki_vr_renderer_pause(naki_vr_renderer_t renderer) {
    if (renderer) static_cast<vr::Renderer*>(renderer)->pause();
}

void naki_vr_renderer_seek(naki_vr_renderer_t renderer, int64_t target_pts_us) {
    if (renderer) static_cast<vr::Renderer*>(renderer)->seek(target_pts_us);
}

void naki_vr_renderer_seek_typed(naki_vr_renderer_t renderer, int64_t target_pts_us, int type) {
    if (renderer) {
        auto seek_type = static_cast<vr::SeekType>(type);
        static_cast<vr::Renderer*>(renderer)->seek(target_pts_us, seek_type);
    }
}

void naki_vr_renderer_set_speed(naki_vr_renderer_t renderer, double speed) {
    if (renderer) static_cast<vr::Renderer*>(renderer)->set_speed(speed);
}

/* ---- Frame stepping ---- */

void naki_vr_renderer_step_forward(naki_vr_renderer_t renderer) {
    if (renderer) static_cast<vr::Renderer*>(renderer)->step_forward();
}

void naki_vr_renderer_step_backward(naki_vr_renderer_t renderer) {
    if (renderer) static_cast<vr::Renderer*>(renderer)->step_backward();
}

/* ---- Query ---- */

int naki_vr_renderer_is_playing(naki_vr_renderer_t renderer) {
    return renderer && static_cast<vr::Renderer*>(renderer)->is_playing() ? 1 : 0;
}

int naki_vr_renderer_is_initialized(naki_vr_renderer_t renderer) {
    return renderer && static_cast<vr::Renderer*>(renderer)->is_initialized() ? 1 : 0;
}

int64_t naki_vr_renderer_current_pts_us(naki_vr_renderer_t renderer) {
    return renderer ? static_cast<vr::Renderer*>(renderer)->current_pts_us() : 0;
}

double naki_vr_renderer_current_speed(naki_vr_renderer_t renderer) {
    return renderer ? static_cast<vr::Renderer*>(renderer)->current_speed() : 1.0;
}

int naki_vr_renderer_track_count(naki_vr_renderer_t renderer) {
    return renderer ? static_cast<int>(static_cast<vr::Renderer*>(renderer)->track_count()) : 0;
}

int64_t naki_vr_renderer_duration_us(naki_vr_renderer_t renderer) {
    return renderer ? static_cast<vr::Renderer*>(renderer)->duration_us() : 0;
}

/* ---- Layout ---- */

void naki_vr_renderer_apply_layout(naki_vr_renderer_t renderer, const naki_vr_layout_state_t* state) {
    if (!renderer || !state) return;
    vr::LayoutState ls;
    ls.mode = state->mode;
    ls.split_pos = state->split_pos;
    ls.zoom_ratio = state->zoom_ratio;
    ls.view_offset[0] = state->view_offset[0];
    ls.view_offset[1] = state->view_offset[1];
    for (int i = 0; i < 4; ++i) ls.order[i] = state->order[i];
    static_cast<vr::Renderer*>(renderer)->apply_layout(ls);
}

void naki_vr_renderer_layout(naki_vr_renderer_t renderer, naki_vr_layout_state_t* out_state) {
    if (!renderer || !out_state) return;
    auto ls = static_cast<vr::Renderer*>(renderer)->layout();
    out_state->mode = ls.mode;
    out_state->split_pos = ls.split_pos;
    out_state->zoom_ratio = ls.zoom_ratio;
    out_state->view_offset[0] = ls.view_offset[0];
    out_state->view_offset[1] = ls.view_offset[1];
    for (int i = 0; i < 4; ++i) out_state->order[i] = ls.order[i];
}

/* ---- Logging & Crash ---- */

void naki_vr_configure_logging(const naki_vr_log_config_t* config) {
    if (!config) return;
    vr::configure_logging(to_log_config(*config));
}

void naki_vr_install_crash_handler(const char* crash_dir) {
    vr::install_crash_handler(crash_dir ? crash_dir : "");
}

void naki_vr_remove_crash_handler(void) {
    vr::remove_crash_handler();
}
