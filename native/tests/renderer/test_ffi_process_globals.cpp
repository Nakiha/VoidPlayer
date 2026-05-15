#include <catch2/catch_test_macros.hpp>

#include "video_renderer/exports/ffi_process_globals.h"
#include "video_renderer/exports/ffi_player_registry.h"

#include <string>

using namespace vr::ffi;

TEST_CASE("FfiProcessGlobals: logging command validates null config",
          "[ffi][process_globals]") {
    REQUIRE(configure_logging_process_command(nullptr) == NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(g_last_error.status == NAKI_VR_ERR_INVALID_ARGUMENT);

    char error[128] = {};
    copy_error(g_last_error, error, sizeof(error));
    REQUIRE(std::string(error).find("log config is required") != std::string::npos);

    set_ok();
}

TEST_CASE("FfiProcessGlobals: crash handler commands keep process-global behavior explicit",
          "[ffi][process_globals]") {
    REQUIRE(install_crash_handler_process_command(nullptr) == NAKI_VR_OK);
    REQUIRE(g_last_error.status == NAKI_VR_OK);
    REQUIRE(remove_crash_handler_process_command() == NAKI_VR_OK);
    REQUIRE(g_last_error.status == NAKI_VR_OK);
}
