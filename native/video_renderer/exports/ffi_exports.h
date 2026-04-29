#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#   ifdef NAKI_VR_FFI_BUILDING
#       define NAKI_VR_FFI_EXPORT __declspec(dllexport)
#   else
#       define NAKI_VR_FFI_EXPORT __declspec(dllimport)
#   endif
#else
#   define NAKI_VR_FFI_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
#   define NAKI_VR_FFI_NOEXCEPT noexcept
extern "C" {
#else
#   define NAKI_VR_FFI_NOEXCEPT
#endif

/* Opaque handle to vr::Renderer */
typedef void* naki_vr_renderer_t;

/* ---- Config structs ---- */

typedef struct naki_vr_log_config_t {
    const char* pattern;    /* Default: "[%Y-%m-%d %H:%M:%S.%e] [%l] %v" */
    const char* file_path;  /* Empty string = no file logging */
    size_t max_file_size;   /* Default: 5MB, 0 = unlimited */
    int max_files;          /* Default: 3, 0 = no rotation */
    int level;              /* spdlog::level::level_enum value: 0=trace..6=off */
} naki_vr_log_config_t;

typedef struct naki_vr_renderer_config_t {
    const char** video_paths; /* Null-terminated array of file paths */
    int64_t hwnd;             /* Window handle (HWND cast to int64_t) */
    int width;                /* Default: 1920 */
    int height;               /* Default: 1080 */
    int use_hardware_decode;  /* 0 = false, 1 = true */
    naki_vr_log_config_t log_config;
} naki_vr_renderer_config_t;

/* ---- Lifecycle ---- */

NAKI_VR_FFI_EXPORT naki_vr_renderer_t naki_vr_renderer_create(void) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_renderer_destroy(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;

NAKI_VR_FFI_EXPORT int naki_vr_renderer_initialize(naki_vr_renderer_t renderer, const naki_vr_renderer_config_t* config) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_renderer_shutdown(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;

/* ---- Playback ---- */

#define NAKI_VR_SEEK_KEYFRAME 0
#define NAKI_VR_SEEK_EXACT    1

NAKI_VR_FFI_EXPORT void naki_vr_renderer_play(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_renderer_pause(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_renderer_seek(naki_vr_renderer_t renderer, int64_t target_pts_us) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_renderer_seek_typed(naki_vr_renderer_t renderer, int64_t target_pts_us, int type) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_renderer_set_speed(naki_vr_renderer_t renderer, double speed) NAKI_VR_FFI_NOEXCEPT;

/* ---- Frame stepping (pause + advance/retreat) ---- */

NAKI_VR_FFI_EXPORT void naki_vr_renderer_step_forward(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_renderer_step_backward(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;

/* ---- Query ---- */

NAKI_VR_FFI_EXPORT int naki_vr_renderer_is_playing(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT int naki_vr_renderer_is_initialized(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT int64_t naki_vr_renderer_current_pts_us(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT double naki_vr_renderer_current_speed(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT int naki_vr_renderer_track_count(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT int64_t naki_vr_renderer_duration_us(naki_vr_renderer_t renderer) NAKI_VR_FFI_NOEXCEPT;

/* ---- Dynamic track management ---- */

NAKI_VR_FFI_EXPORT int naki_vr_renderer_add_track(naki_vr_renderer_t renderer, const char* video_path) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_renderer_remove_track(naki_vr_renderer_t renderer, int file_id) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT int naki_vr_renderer_has_track(naki_vr_renderer_t renderer, int slot) NAKI_VR_FFI_NOEXCEPT;

/* ---- Layout ---- */

#define NAKI_VR_LAYOUT_SIDE_BY_SIDE 0
#define NAKI_VR_LAYOUT_SPLIT_SCREEN 1

typedef struct naki_vr_layout_state_t {
    int mode;                     /* 0=SIDE_BY_SIDE, 1=SPLIT_SCREEN */
    float split_pos;              /* Split divider position (0.0-1.0) */
    float zoom_ratio;             /* 1.0=fit, >1.0=zoom in */
    float view_offset[2];         /* Pan offset [x, y] in pixel coordinates */
    int order[4];                 /* Track display order mapping */
} naki_vr_layout_state_t;

NAKI_VR_FFI_EXPORT void naki_vr_renderer_apply_layout(naki_vr_renderer_t renderer, const naki_vr_layout_state_t* state) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_renderer_layout(naki_vr_renderer_t renderer, naki_vr_layout_state_t* out_state) NAKI_VR_FFI_NOEXCEPT;

/* ---- Logging & Crash ---- */

NAKI_VR_FFI_EXPORT void naki_vr_configure_logging(const naki_vr_log_config_t* config) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_install_crash_handler(const char* crash_dir) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_remove_crash_handler(void) NAKI_VR_FFI_NOEXCEPT;

#ifdef __cplusplus
}
#endif
