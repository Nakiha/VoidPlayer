#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/decode_preroll_policy.h"

using namespace vr;

TEST_CASE("DecodePrerollPolicy: post-seek target waits one extra hardware frame",
          "[decode_thread][decode_preroll_policy]") {
    REQUIRE(decode_post_seek_preroll_target(false) == 1);
    REQUIRE(decode_post_seek_preroll_target(true) == 2);

    REQUIRE(is_decode_preroll_ready(true, false, 1, false));
    REQUIRE_FALSE(is_decode_preroll_ready(true, true, 1, true));
    REQUIRE(is_decode_preroll_ready(true, true, 2, false));
}

TEST_CASE("DecodePrerollPolicy: normal preroll delegates to buffer readiness",
          "[decode_thread][decode_preroll_policy]") {
    REQUIRE_FALSE(is_decode_preroll_ready(false, false, 4, false));
    REQUIRE(is_decode_preroll_ready(false, false, 0, true));
    REQUIRE(is_decode_preroll_ready(false, true, 0, true));
}

TEST_CASE("DecodePrerollPolicy: buffering transitions to ready when preroll is ready",
          "[decode_thread][decode_preroll_policy]") {
    auto decision = choose_decode_preroll_transition(
        TrackState::Buffering,
        true,
        true);

    REQUIRE(decision.complete);
    REQUIRE(decision.output_state == TrackState::Ready);
    REQUIRE(decision.pause_decode);
    REQUIRE(decision.clear_post_seek);

    decision = choose_decode_preroll_transition(
        TrackState::Buffering,
        true,
        false);
    REQUIRE(decision.complete);
    REQUIRE_FALSE(decision.pause_decode);
}

TEST_CASE("DecodePrerollPolicy: non-ready or non-buffering states do not transition",
          "[decode_thread][decode_preroll_policy]") {
    auto decision = choose_decode_preroll_transition(
        TrackState::Buffering,
        false,
        true);
    REQUIRE_FALSE(decision.complete);
    REQUIRE(decision.output_state == TrackState::Buffering);
    REQUIRE_FALSE(decision.pause_decode);
    REQUIRE_FALSE(decision.clear_post_seek);

    decision = choose_decode_preroll_transition(
        TrackState::Ready,
        true,
        true);
    REQUIRE_FALSE(decision.complete);
    REQUIRE(decision.output_state == TrackState::Ready);
}
