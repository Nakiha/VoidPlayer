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

#define NAKI_VR_ABI_VERSION 1u

typedef enum naki_vr_status_t {
    NAKI_VR_OK = 0,
    NAKI_VR_ERR_INVALID_ARGUMENT = 1,
    NAKI_VR_ERR_NOT_INITIALIZED = 2,
    NAKI_VR_ERR_OPEN_FAILED = 3,
    NAKI_VR_ERR_INTERNAL = 1000
} naki_vr_status_t;

/* Opaque handle to vr::NativePlayer. */
typedef void* naki_vr_player_t;

/* ---- Config structs ---- */

typedef struct naki_vr_log_config_t {
    uint32_t size;          /* sizeof(naki_vr_log_config_t) */
    uint32_t abi_version;   /* NAKI_VR_ABI_VERSION */
    const char* pattern;    /* Default: "[%Y-%m-%d %H:%M:%S.%e] [%l] %v" */
    const char* file_path;  /* Empty string = no file logging */
    size_t max_file_size;   /* Default: 5MB, 0 = unlimited */
    int max_files;          /* Default: 3, 0 = no rotation */
    int level;              /* NAKI_VR_LOG_* value: 0=trace..6=off */
} naki_vr_log_config_t;

#define NAKI_VR_LOG_TRACE    0
#define NAKI_VR_LOG_DEBUG    1
#define NAKI_VR_LOG_INFO     2
#define NAKI_VR_LOG_WARN     3
#define NAKI_VR_LOG_ERROR    4
#define NAKI_VR_LOG_CRITICAL 5
#define NAKI_VR_LOG_OFF      6

typedef struct naki_vr_player_config_t {
    uint32_t size;            /* sizeof(naki_vr_player_config_t) */
    uint32_t abi_version;     /* NAKI_VR_ABI_VERSION */
    const char** video_paths; /* Null-terminated array of file paths */
    int64_t hwnd;             /* Window handle (HWND cast to int64_t) */
    int width;                /* Default: 1920 */
    int height;               /* Default: 1080 */
    int use_hardware_decode;  /* 0 = false, 1 = true */
    naki_vr_log_config_t log_config;
} naki_vr_player_config_t;

/* ---- Lifecycle ---- */

NAKI_VR_FFI_EXPORT uint32_t naki_vr_abi_version(void) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT naki_vr_status_t naki_vr_last_error(naki_vr_player_t player, char* buf, size_t cap) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT naki_vr_player_t naki_vr_player_create(void) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_destroy(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT int naki_vr_player_initialize(naki_vr_player_t player, const naki_vr_player_config_t* config) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_shutdown(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;

/* ---- Playback ---- */

#define NAKI_VR_SEEK_KEYFRAME 0
#define NAKI_VR_SEEK_EXACT    1

NAKI_VR_FFI_EXPORT void naki_vr_player_play(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_pause(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_seek(naki_vr_player_t player, int64_t target_pts_us) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_seek_typed(naki_vr_player_t player, int64_t target_pts_us, int type) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_set_speed(naki_vr_player_t player, double speed) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_set_loop_range(naki_vr_player_t player, int enabled, int64_t start_us, int64_t end_us) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_set_audible_track(naki_vr_player_t player, int file_id) NAKI_VR_FFI_NOEXCEPT;

/* ---- Frame stepping (pause + advance/retreat) ---- */

NAKI_VR_FFI_EXPORT void naki_vr_player_step_forward(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_step_backward(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;

/* ---- Query ---- */

NAKI_VR_FFI_EXPORT int naki_vr_player_is_playing(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT int naki_vr_player_is_initialized(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT int64_t naki_vr_player_current_pts_us(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT double naki_vr_player_current_speed(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT int naki_vr_player_track_count(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT int64_t naki_vr_player_duration_us(naki_vr_player_t player) NAKI_VR_FFI_NOEXCEPT;

/* ---- Dynamic track management ---- */

NAKI_VR_FFI_EXPORT int naki_vr_player_add_track(naki_vr_player_t player, const char* video_path) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_remove_track(naki_vr_player_t player, int file_id) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT int naki_vr_player_has_track(naki_vr_player_t player, int slot) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_set_track_offset(naki_vr_player_t player, int file_id, int64_t offset_us) NAKI_VR_FFI_NOEXCEPT;

/* ---- Layout ---- */

#define NAKI_VR_LAYOUT_SIDE_BY_SIDE 0
#define NAKI_VR_LAYOUT_SPLIT_SCREEN 1
#define NAKI_VR_PIXEL_SIZE_UNIFORM_VIDEO_PIXELS 0
#define NAKI_VR_PIXEL_SIZE_FILL_VIEW 1

typedef struct naki_vr_player_layout_state_t {
    uint32_t size;                 /* sizeof(naki_vr_player_layout_state_t) */
    uint32_t abi_version;          /* NAKI_VR_ABI_VERSION */
    int mode;                     /* 0=SIDE_BY_SIDE, 1=SPLIT_SCREEN */
    float split_pos;              /* Split divider position (0.0-1.0) */
    float zoom_ratio;             /* 1.0=fit, >1.0=zoom in */
    float view_offset[2];         /* Pan offset [x, y] in pixel coordinates */
    int pixel_size_mode;          /* 0=uniform video pixels, 1=fit each view slot */
    int order[4];                 /* Track display order mapping */
} naki_vr_player_layout_state_t;

NAKI_VR_FFI_EXPORT void naki_vr_player_apply_layout(naki_vr_player_t player, const naki_vr_player_layout_state_t* state) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_player_layout(naki_vr_player_t player, naki_vr_player_layout_state_t* out_state) NAKI_VR_FFI_NOEXCEPT;

/* ---- Logging & Crash ---- */

NAKI_VR_FFI_EXPORT void naki_vr_configure_logging(const naki_vr_log_config_t* config) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_install_crash_handler(const char* crash_dir) NAKI_VR_FFI_NOEXCEPT;
NAKI_VR_FFI_EXPORT void naki_vr_remove_crash_handler(void) NAKI_VR_FFI_NOEXCEPT;

#ifdef __cplusplus
}
#endif
