#include <catch2/catch_test_macros.hpp>

#include "video_renderer/exports/ffi_player_lifecycle.h"
#include "video_renderer/exports/ffi_player_registry.h"

#include <string>

using namespace vr::ffi;

TEST_CASE("FfiPlayerLifecycle: create destroy and double destroy stay explicit",
          "[ffi][lifecycle]") {
    auto player = create_player_lifecycle_command();
    REQUIRE(player != nullptr);
    REQUIRE(g_last_error.status == NAKI_VR_OK);

    char error[128] = {};
    REQUIRE(copy_player_error_lifecycle_command(player, error, sizeof(error)) == NAKI_VR_OK);
    REQUIRE(std::string(error).empty());

    REQUIRE(destroy_player_lifecycle_command(player) == NAKI_VR_OK);
    REQUIRE(g_last_error.status == NAKI_VR_OK);
    REQUIRE(destroy_player_lifecycle_command(player) == NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(g_last_error.status == NAKI_VR_ERR_INVALID_ARGUMENT);

    REQUIRE(destroy_player_lifecycle_command(nullptr) == NAKI_VR_OK);
}

TEST_CASE("FfiPlayerLifecycle: error copy preserves global fallback",
          "[ffi][lifecycle]") {
    set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "unit lifecycle error");

    char error[128] = {};
    REQUIRE(copy_global_error_lifecycle_command(nullptr, error, sizeof(error)) ==
            NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(std::string(error).find("unit lifecycle error") != std::string::npos);

    char player_error[128] = {};
    REQUIRE(copy_player_error_lifecycle_command(nullptr, player_error, sizeof(player_error)) ==
            NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(std::string(player_error).find("player is required") != std::string::npos);

    set_ok();
}

TEST_CASE("FfiPlayerLifecycle: initialize wrappers validate null configs",
          "[ffi][lifecycle]") {
    auto player = create_player_lifecycle_command();
    REQUIRE(player != nullptr);

    REQUIRE(initialize_player_v2_lifecycle_command(player, nullptr) ==
            NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(g_last_error.status == NAKI_VR_ERR_INVALID_ARGUMENT);

    REQUIRE(initialize_player_v1_lifecycle_command(player, nullptr) == 0);
    REQUIRE(g_last_error.status == NAKI_VR_ERR_INVALID_ARGUMENT);

    REQUIRE(shutdown_player_lifecycle_command(player) == NAKI_VR_OK);
    REQUIRE(destroy_player_lifecycle_command(player) == NAKI_VR_OK);
}
