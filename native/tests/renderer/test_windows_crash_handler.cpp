#include <catch2/catch_test_macros.hpp>

#include "windows/common/windows_crash_handler.h"

#include <filesystem>

TEST_CASE("windows crash handler install/remove is idempotent", "[windows_crash_handler]") {
    vr::remove_windows_crash_handler();
    REQUIRE_FALSE(vr::windows_crash_handler_installed());

    vr::WindowsCrashHandlerConfig config;
    config.crash_dir =
        (std::filesystem::temp_directory_path() / "voidplayer_crash_handler_test").string();
    config.install_crt_handlers = false;

    REQUIRE(vr::install_windows_crash_handler(config));
    REQUIRE(vr::windows_crash_handler_installed());

    REQUIRE(vr::install_windows_crash_handler(config));
    REQUIRE(vr::windows_crash_handler_installed());

    vr::remove_windows_crash_handler();
    REQUIRE_FALSE(vr::windows_crash_handler_installed());

    vr::remove_windows_crash_handler();
    REQUIRE_FALSE(vr::windows_crash_handler_installed());
}

TEST_CASE("legacy crash handler API can reinstall after remove", "[windows_crash_handler]") {
    vr::remove_crash_handler();
    REQUIRE_FALSE(vr::windows_crash_handler_installed());

    vr::install_crash_handler("");
    REQUIRE(vr::windows_crash_handler_installed());

    vr::remove_crash_handler();
    REQUIRE_FALSE(vr::windows_crash_handler_installed());

    vr::install_crash_handler("");
    REQUIRE(vr::windows_crash_handler_installed());

    vr::remove_crash_handler();
    REQUIRE_FALSE(vr::windows_crash_handler_installed());
}
