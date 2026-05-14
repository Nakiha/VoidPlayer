/* test_ffi_c.c - Pure C validation of the FFI interface.
 *
 * This file MUST compile as standard C (not C++) to prove the extern "C"
 * boundary is clean. It exercises every exported function that can be called
 * without a valid HWND / initialized player.
 */

#include "video_renderer/exports/ffi_exports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#endif

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                             \
    if (cond) {                                           \
        g_pass++;                                         \
        printf("  PASS  %s\n", msg);                      \
    } else {                                              \
        g_fail++;                                         \
        printf("  FAIL  %s  (line %d)\n", msg, __LINE__); \
    }                                                     \
} while (0)

static void init_log_config(naki_vr_log_config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->size = sizeof(*cfg);
    cfg->abi_version = NAKI_VR_ABI_VERSION;
    cfg->level = NAKI_VR_LOG_INFO;
}

static void init_player_config(naki_vr_player_config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->size = sizeof(*cfg);
    cfg->abi_version = NAKI_VR_ABI_VERSION;
    cfg->width = 1920;
    cfg->height = 1080;
    cfg->use_hardware_decode = 1;
    init_log_config(&cfg->log_config);
}

static void init_player_config_v2(naki_vr_player_config_v2_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->size = sizeof(*cfg);
    cfg->abi_version = NAKI_VR_ABI_VERSION;
    cfg->width = 1920;
    cfg->height = 1080;
    cfg->use_hardware_decode = 1;
    init_log_config(&cfg->log_config);
}

static void init_layout_state(naki_vr_player_layout_state_t* state) {
    memset(state, 0, sizeof(*state));
    state->size = sizeof(*state);
    state->abi_version = NAKI_VR_ABI_VERSION;
    state->mode = NAKI_VR_LAYOUT_SIDE_BY_SIDE;
    state->split_pos = 0.5f;
    state->zoom_ratio = 1.0f;
    state->pixel_size_mode = NAKI_VR_PIXEL_SIZE_UNIFORM_VIDEO_PIXELS;
    state->order[0] = 0;
    state->order[1] = 1;
    state->order[2] = 2;
    state->order[3] = 3;
}

static naki_vr_status_t last_error(char* buf, size_t cap) {
    return naki_vr_last_error(NULL, buf, cap);
}

#ifdef _WIN32
typedef struct FfiReaderArgs {
    naki_vr_player_t player;
    volatile LONG* stop;
    volatile LONG* calls;
} FfiReaderArgs;

typedef struct FfiWriterArgs {
    naki_vr_player_t player;
    volatile LONG* stop;
    volatile LONG* calls;
} FfiWriterArgs;

static DWORD WINAPI ffi_reader_thread(LPVOID param) {
    FfiReaderArgs* args = (FfiReaderArgs*)param;
    while (InterlockedCompareExchange(args->stop, 0, 0) == 0) {
        (void)naki_vr_player_is_initialized(args->player);
        InterlockedIncrement(args->calls);
    }
    return 0;
}

static DWORD WINAPI ffi_writer_thread(LPVOID param) {
    FfiWriterArgs* args = (FfiWriterArgs*)param;
    while (InterlockedCompareExchange(args->stop, 0, 0) == 0) {
        (void)naki_vr_player_set_speed_status(args->player, 1.0);
        (void)naki_vr_player_set_loop_range_status(args->player, 0, 0, 0);
        (void)naki_vr_player_get_error(args->player, NULL, 0);
        InterlockedIncrement(args->calls);
    }
    return 0;
}
#endif

int main(void) {
    printf("=== C FFI Validation ===\n\n");

    CHECK(naki_vr_abi_version() == NAKI_VR_ABI_VERSION, "ABI version matches header");
    CHECK(sizeof(naki_vr_log_config_t) >= offsetof(naki_vr_log_config_t, level) + sizeof(int),
          "log config exposes size/version-prefixed layout");
    CHECK(sizeof(naki_vr_player_config_t) >= offsetof(naki_vr_player_config_t, log_config) + sizeof(naki_vr_log_config_t),
          "player config exposes size/version-prefixed layout");
    CHECK(sizeof(naki_vr_player_config_v2_t) >= offsetof(naki_vr_player_config_v2_t, reserved) + sizeof(uint64_t) * 4,
          "player config v2 exposes counted-path layout");
    CHECK(sizeof(naki_vr_player_layout_state_t) >= offsetof(naki_vr_player_layout_state_t, order) + sizeof(int) * 4,
          "layout config exposes size/version-prefixed layout");

    /* ---- configure_logging ---- */
    {
        naki_vr_log_config_t cfg;
        init_log_config(&cfg);
        cfg.pattern = "[%l] %v";
        cfg.level = NAKI_VR_LOG_WARN;
        cfg.max_file_size = 1024;
        cfg.max_files = 1;
        naki_vr_configure_logging(&cfg);
        CHECK(1, "naki_vr_configure_logging");

        cfg.level = 999;
        naki_vr_configure_logging(&cfg);
        CHECK(last_error(NULL, 0) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "invalid log level reports invalid argument");
    }

    /* ---- create / destroy ---- */
    {
        naki_vr_player_t p = naki_vr_player_create();
        CHECK(p != NULL, "naki_vr_player_create returns non-NULL");

        /* Fresh player: no init, so all queries should return defaults */
        CHECK(naki_vr_player_is_playing(p) == 0, "is_playing == 0 (fresh)");
        CHECK(naki_vr_player_is_initialized(p) == 0, "is_initialized == 0 (fresh)");
        CHECK(naki_vr_player_current_pts_us(p) == 0, "current_pts_us == 0 (fresh)");
        CHECK(naki_vr_player_track_count(p) == 0, "track_count == 0 (fresh)");
        CHECK(naki_vr_player_duration_us(p) == 0, "duration_us == 0 (fresh)");

        /* Default speed is 1.0 */
        double spd = naki_vr_player_current_speed(p);
        CHECK(spd > 0.999 && spd < 1.001, "current_speed == 1.0 (fresh)");

        /* Safe to call shutdown on uninitialized player */
        naki_vr_player_shutdown(p);
        CHECK(1, "shutdown on uninitialized player (no crash)");

        /* Safe operations on uninitialized player */
        naki_vr_player_play(p);
        naki_vr_player_pause(p);
        naki_vr_player_seek(p, 0);
        naki_vr_player_seek_typed(p, 0, NAKI_VR_SEEK_EXACT);
        naki_vr_player_seek_typed(p, 0, 999);
        CHECK(last_error(NULL, 0) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "invalid seek type reports invalid argument");
        naki_vr_player_set_speed(p, 2.0);
        CHECK(naki_vr_player_set_speed_status(p, 2.0) == NAKI_VR_OK,
              "status API returns OK for set_speed");
        naki_vr_player_set_loop_range(p, 0, 0, 0);
        CHECK(naki_vr_player_set_loop_range_status(p, 1, -1, 0) ==
                  NAKI_VR_ERR_INVALID_ARGUMENT,
              "status API rejects invalid loop range");
        CHECK(naki_vr_player_get_error(p, NULL, 0) == NAKI_VR_OK,
              "per-player error remains OK for pre-handle validation failures");
        naki_vr_player_set_audible_track(p, -1);
        naki_vr_player_set_track_offset(p, 1, 0);
        {
            naki_vr_player_layout_state_t layout;
            init_layout_state(&layout);
            naki_vr_player_apply_layout(p, &layout);
            naki_vr_player_layout(p, &layout);
            CHECK(layout.size == sizeof(layout) && layout.abi_version == NAKI_VR_ABI_VERSION,
                  "layout roundtrip preserves ABI metadata");
        }
        CHECK(1, "playback ops on uninitialized player (no crash)");

        naki_vr_player_destroy(p);
        CHECK(1, "naki_vr_player_destroy");
        naki_vr_player_destroy(p);
        CHECK(last_error(NULL, 0) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "double destroy reports invalid argument without crashing");
        CHECK(naki_vr_player_is_initialized(p) == 0, "destroyed handle query returns default");
        CHECK(last_error(NULL, 0) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "destroyed handle query reports invalid argument");
    }

#ifdef _WIN32
    /* ---- concurrent destroy / query smoke ---- */
    {
        enum { kReaderThreadCount = 4, kWriterThreadCount = 2, kThreadCount = 6 };
        naki_vr_player_t p = naki_vr_player_create();
        volatile LONG stop = 0;
        volatile LONG reader_calls = 0;
        volatile LONG writer_calls = 0;
        HANDLE threads[kThreadCount];
        FfiReaderArgs reader_args;
        FfiWriterArgs writer_args;
        int created = 0;

        reader_args.player = p;
        reader_args.stop = &stop;
        reader_args.calls = &reader_calls;
        writer_args.player = p;
        writer_args.stop = &stop;
        writer_args.calls = &writer_calls;

        for (int i = 0; i < kReaderThreadCount; ++i) {
            HANDLE thread = CreateThread(NULL, 0, ffi_reader_thread, &reader_args, 0, NULL);
            if (thread != NULL) {
                threads[created++] = thread;
            }
        }
        for (int i = 0; i < kWriterThreadCount; ++i) {
            HANDLE thread = CreateThread(NULL, 0, ffi_writer_thread, &writer_args, 0, NULL);
            if (thread != NULL) {
                threads[created++] = thread;
            }
        }

        Sleep(10);
        naki_vr_player_destroy(p);
        Sleep(10);
        InterlockedExchange(&stop, 1);

        if (created > 0) {
            WaitForMultipleObjects((DWORD)created, threads, TRUE, 5000);
        }
        for (int i = 0; i < created; ++i) {
            CloseHandle(threads[i]);
        }

        CHECK(created == kThreadCount, "concurrent FFI smoke created reader/writer threads");
        CHECK(reader_calls > 0, "concurrent FFI smoke exercised reader calls");
        CHECK(writer_calls > 0, "concurrent FFI smoke exercised writer calls");
        CHECK(naki_vr_player_is_initialized(p) == 0,
              "query after concurrent destroy returns default");
        CHECK(last_error(NULL, 0) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "query after concurrent destroy reports invalid argument");
    }
#endif

    /* ---- NULL safety ---- */
    {
        naki_vr_player_initialize(NULL, NULL);
        CHECK(1, "initialize(NULL, NULL) does not crash");
        CHECK(last_error(NULL, 0) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "initialize(NULL, NULL) reports invalid argument");

        {
            char err[128];
            naki_vr_status_t st = last_error(err, sizeof(err));
            CHECK(st == NAKI_VR_ERR_INVALID_ARGUMENT && strlen(err) > 0,
                  "last_error copies a diagnostic message");
        }

        naki_vr_player_destroy(NULL);
        CHECK(1, "destroy(NULL) does not crash");

        naki_vr_configure_logging(NULL);
        CHECK(1, "configure_logging(NULL) does not crash");

        naki_vr_install_crash_handler(NULL);
        CHECK(1, "install_crash_handler(NULL) does not crash");
    }

    /* ---- config validation ---- */
    {
        naki_vr_player_t p = naki_vr_player_create();
        naki_vr_player_config_t cfg;
        init_player_config(&cfg);
        cfg.abi_version = 999;
        CHECK(naki_vr_player_initialize(p, &cfg) == 0,
              "initialize rejects ABI version mismatch");
        CHECK(last_error(NULL, 0) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "ABI version mismatch reports invalid argument");
        naki_vr_player_destroy(p);
    }

    {
        naki_vr_player_t p = naki_vr_player_create();
        naki_vr_player_config_v2_t cfg;
        const char* paths[] = {"C:/definitely/missing.mp4"};
        init_player_config_v2(&cfg);
        cfg.hwnd = 1;
        cfg.video_paths = paths;
        cfg.video_path_count = 1;
        CHECK(naki_vr_player_initialize_v2(p, &cfg) == NAKI_VR_ERR_OPEN_FAILED,
              "initialize_v2 uses counted paths and returns status");
        CHECK(naki_vr_player_get_error(p, NULL, 0) == NAKI_VR_ERR_OPEN_FAILED,
              "per-player error records initialize_v2 failure");
        naki_vr_player_destroy(p);
    }

    {
        naki_vr_player_t p = naki_vr_player_create();
        naki_vr_player_config_v2_t cfg;
        const char* paths[] = {"a", "b", "c", "d", "e"};
        init_player_config_v2(&cfg);
        cfg.hwnd = 1;
        cfg.video_paths = paths;
        cfg.video_path_count = 5;
        CHECK(naki_vr_player_initialize_v2(p, &cfg) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "initialize_v2 rejects too many counted paths");
        naki_vr_player_destroy(p);
    }

    {
        naki_vr_player_t p = naki_vr_player_create();
        int slot = 123;
        CHECK(naki_vr_player_add_track_status(p, NULL, &slot) ==
                  NAKI_VR_ERR_INVALID_ARGUMENT,
              "add_track_status rejects null path");
        CHECK(slot == -1, "add_track_status clears output slot on failure");
        naki_vr_player_destroy(p);
    }

    {
        naki_vr_player_t p = naki_vr_player_create();
        naki_vr_player_config_t cfg;
        const char* empty_path[] = {"", NULL};
        init_player_config(&cfg);
        cfg.hwnd = 1;
        cfg.video_paths = empty_path;
        CHECK(naki_vr_player_initialize(p, &cfg) == 0,
              "initialize rejects empty video path");
        CHECK(last_error(NULL, 0) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "empty video path reports invalid argument");
        naki_vr_player_destroy(p);
    }

    {
        naki_vr_player_t p = naki_vr_player_create();
        naki_vr_player_config_t cfg;
        const char bad_utf8[] = {(char)0xC3, (char)0x28, 0};
        const char* paths[] = {bad_utf8, NULL};
        init_player_config(&cfg);
        cfg.hwnd = 1;
        cfg.video_paths = paths;
        CHECK(naki_vr_player_initialize(p, &cfg) == 0,
              "initialize rejects malformed UTF-8 path");
        CHECK(last_error(NULL, 0) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "malformed UTF-8 path reports invalid argument");
        naki_vr_player_destroy(p);
    }

    {
        naki_vr_player_t p = naki_vr_player_create();
        naki_vr_player_config_t cfg;
        const char* too_many_paths[] = {"a", "b", "c", "d", "e", NULL};
        init_player_config(&cfg);
        cfg.hwnd = 1;
        cfg.video_paths = too_many_paths;
        CHECK(naki_vr_player_initialize(p, &cfg) == 0,
              "initialize rejects too many video paths");
        CHECK(last_error(NULL, 0) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "too many video paths reports invalid argument");
        naki_vr_player_destroy(p);
    }

    {
        naki_vr_player_t p = naki_vr_player_create();
        naki_vr_player_config_t cfg;
        init_player_config(&cfg);
        cfg.width = 20000;
        CHECK(naki_vr_player_initialize(p, &cfg) == 0,
              "initialize rejects oversized dimensions");
        CHECK(last_error(NULL, 0) == NAKI_VR_ERR_INVALID_ARGUMENT,
              "oversized dimensions report invalid argument");
        naki_vr_player_destroy(p);
    }

    /* ---- crash handler lifecycle ---- */
    {
        naki_vr_install_crash_handler(".");
        CHECK(1, "naki_vr_install_crash_handler");
        naki_vr_remove_crash_handler();
        CHECK(1, "naki_vr_remove_crash_handler");
    }

    /* ---- Summary ---- */
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
