#include <catch2/catch_test_macros.hpp>

#include "common/win_utf8.h"

using namespace vr::win_utf8;

TEST_CASE("win_utf8 rejects malformed UTF-8", "[win_utf8]") {
    const std::string invalid = std::string("\xC3\x28", 2);
    REQUIRE_FALSE(is_valid_utf8(invalid));

#ifdef _WIN32
    const auto result = try_utf16_from_utf8(invalid);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error != 0);
#endif
}

TEST_CASE("win_utf8 round-trips valid UTF-8 paths", "[win_utf8]") {
    const std::string valid =
        std::string("C:/") + "\xE8\xA7\x86\xE9\xA2\x91" + "/test.mp4";
    REQUIRE(is_valid_utf8(valid));

#ifdef _WIN32
    const auto wide = try_utf16_from_utf8(valid);
    REQUIRE(wide.ok);
    REQUIRE_FALSE(wide.value.empty());

    const auto utf8 = try_utf8_from_utf16(wide.value.c_str());
    REQUIRE(utf8.ok);
    REQUIRE(utf8.value == valid);
#endif
}
