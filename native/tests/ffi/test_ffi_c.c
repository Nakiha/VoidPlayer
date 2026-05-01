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

int main(void) {
    printf("=== C FFI Validation ===\n\n");

    /* ---- configure_logging ---- */
    {
        naki_vr_log_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.pattern = "[%l] %v";
        cfg.level = 3; /* spdlog::level::warn */
        cfg.max_file_size = 1024;
        cfg.max_files = 1;
        naki_vr_configure_logging(&cfg);
        CHECK(1, "naki_vr_configure_logging");
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
        naki_vr_player_set_speed(p, 2.0);
        naki_vr_player_set_loop_range(p, 0, 0, 0);
        naki_vr_player_set_audible_track(p, -1);
        naki_vr_player_set_track_offset(p, 1, 0);
        CHECK(1, "playback ops on uninitialized player (no crash)");

        naki_vr_player_destroy(p);
        CHECK(1, "naki_vr_player_destroy");
    }

    /* ---- NULL safety ---- */
    {
        naki_vr_player_initialize(NULL, NULL);
        CHECK(1, "initialize(NULL, NULL) does not crash");

        naki_vr_player_destroy(NULL);
        CHECK(1, "destroy(NULL) does not crash");

        naki_vr_configure_logging(NULL);
        CHECK(1, "configure_logging(NULL) does not crash");

        naki_vr_install_crash_handler(NULL);
        CHECK(1, "install_crash_handler(NULL) does not crash");
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
