/* test_ffi_c.c - Pure C validation of the FFI interface.
 *
 * This file MUST compile as standard C (not C++) to prove the extern "C"
 * boundary is clean.  It exercises every exported function that can be
 * called without a valid HWND / initialized renderer.
 */

#include "exports/ffi_exports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                            \
    if (cond) {                                          \
        g_pass++;                                        \
        printf("  PASS  %s\n", msg);                     \
    } else {                                             \
        g_fail++;                                        \
        printf("  FAIL  %s  (line %d)\n", msg, __LINE__); \
    }                                                    \
} while (0)

int main(void) {
    printf("=== C FFI Validation ===\n\n");

    /* ---- configure_logging ---- */
    {
        naki_vr_log_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.pattern = "[%l] %v";
        cfg.level = 2; /* spdlog::level::warn */
        cfg.max_file_size = 1024;
        cfg.max_files = 1;
        naki_vr_configure_logging(&cfg);
        CHECK(1, "naki_vr_configure_logging");
    }

    /* ---- create / destroy ---- */
    {
        naki_vr_renderer_t r = naki_vr_renderer_create();
        CHECK(r != NULL, "naki_vr_renderer_create returns non-NULL");

        /* Fresh renderer: no init, so all queries should return defaults */
        CHECK(naki_vr_renderer_is_playing(r)    == 0, "is_playing == 0 (fresh)");
        CHECK(naki_vr_renderer_is_initialized(r) == 0, "is_initialized == 0 (fresh)");
        CHECK(naki_vr_renderer_current_pts_us(r) == 0, "current_pts_us == 0 (fresh)");
        CHECK(naki_vr_renderer_track_count(r)   == 0, "track_count == 0 (fresh)");
        CHECK(naki_vr_renderer_duration_us(r)   == 0, "duration_us == 0 (fresh)");

        /* Default speed is 1.0 */
        double spd = naki_vr_renderer_current_speed(r);
        CHECK(spd > 0.999 && spd < 1.001, "current_speed == 1.0 (fresh)");

        /* Safe to call shutdown on uninitialized renderer */
        naki_vr_renderer_shutdown(r);
        CHECK(1, "shutdown on uninitialized renderer (no crash)");

        /* Safe operations on uninitialized renderer */
        naki_vr_renderer_pause(r);
        naki_vr_renderer_resume(r);
        naki_vr_renderer_seek(r, 0);
        naki_vr_renderer_set_speed(r, 2.0);
        CHECK(1, "playback ops on uninitialized renderer (no crash)");

        naki_vr_renderer_destroy(r);
        CHECK(1, "naki_vr_renderer_destroy");
    }

    /* ---- NULL safety ---- */
    {
        naki_vr_renderer_initialize(NULL, NULL);
        CHECK(1, "initialize(NULL, NULL) does not crash");

        naki_vr_renderer_destroy(NULL);
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
