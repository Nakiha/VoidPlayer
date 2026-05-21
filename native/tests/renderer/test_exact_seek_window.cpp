#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/exact_seek_window.h"

using namespace vr;

TEST_CASE("ExactSeekWindow: lookbehind gate keeps bounded pre-target candidates",
          "[decode_thread][exact_seek]") {
    constexpr int64_t target = 1000000;

    REQUIRE_FALSE(should_collect_exact_seek_candidate(749999, target));
    REQUIRE(should_collect_exact_seek_candidate(750000, target));
    REQUIRE(should_collect_exact_seek_candidate(999999, target));
    REQUIRE(should_collect_exact_seek_candidate(1000000, target));
    REQUIRE(should_collect_exact_seek_candidate(1250000, target));
    REQUIRE_FALSE(should_collect_exact_seek_candidate(1000000, -1));
}

TEST_CASE("ExactSeekWindow: preview readiness requires a post-target window",
          "[decode_thread][exact_seek]") {
    constexpr int64_t target = 1000000;

    REQUIRE_FALSE(is_exact_seek_preview_window_ready(-1, 4, target));
    REQUIRE_FALSE(is_exact_seek_preview_window_ready(target, 0, target));
    REQUIRE_FALSE(is_exact_seek_preview_window_ready(target, 4, 999999));
    REQUIRE_FALSE(is_exact_seek_preview_window_ready(target, 3, 1000000));
    REQUIRE(is_exact_seek_preview_window_ready(target, 4, 1000000));
    REQUIRE(is_exact_seek_preview_window_ready(target, 4, 1200000));
}

TEST_CASE("ExactSeekWindow: preview selection matches decode-thread fallback",
          "[decode_thread][exact_seek]") {
    REQUIRE_FALSE(select_exact_seek_preview_index({}, 1000000).has_value());
    REQUIRE_FALSE(select_exact_seek_preview_index({1000000}, -1).has_value());

    REQUIRE(select_exact_seek_preview_index({1000000, 1040000}, 1000000) == 0);
    REQUIRE(select_exact_seek_preview_index({900000, 1000000, 1040000}, 1000000) == 1);
    REQUIRE(select_exact_seek_preview_index({700000, 900000, 950000}, 1000000) == 2);
    REQUIRE(select_exact_seek_preview_index({700000, 900000, 1100000, 1140000}, 1000000) == 1);
    REQUIRE(select_exact_seek_preview_index({700000, 1100000, 950000, 900000}, 1000000) == 2);
    REQUIRE(select_exact_seek_preview_index({700000, 1100000, 950000, 900000}, 1000000, true) == 1);
    REQUIRE(select_exact_seek_preview_index({700000, 900000, 950000}, 1000000, true) == 2);
}
