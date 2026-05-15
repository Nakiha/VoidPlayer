#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/exact_seek_publish_policy.h"

using namespace vr;

TEST_CASE("ExactSeekPublishPolicy: publish window is bounded by reorder count and capacity",
          "[decode_thread][exact_seek_publish]") {
    auto window = choose_exact_seek_preview_publish_window(
        1,
        10,
        2,
        5,
        4);

    REQUIRE(window.can_publish);
    REQUIRE(window.end == 4);
    REQUIRE(window.published == 3);

    window = choose_exact_seek_preview_publish_window(
        1,
        3,
        0,
        10,
        4);

    REQUIRE(window.can_publish);
    REQUIRE(window.end == 3);
    REQUIRE(window.published == 2);
}

TEST_CASE("ExactSeekPublishPolicy: publish window rejects invalid or full output",
          "[decode_thread][exact_seek_publish]") {
    REQUIRE_FALSE(choose_exact_seek_preview_publish_window(
                      4, 4, 0, 4, 4).can_publish);
    REQUIRE_FALSE(choose_exact_seek_preview_publish_window(
                      1, 4, 4, 4, 4).can_publish);
    REQUIRE_FALSE(choose_exact_seek_preview_publish_window(
                      1, 4, 5, 4, 4).can_publish);
    REQUIRE_FALSE(choose_exact_seek_preview_publish_window(
                      1, 4, 0, 4, 0).can_publish);
}

TEST_CASE("ExactSeekPublishPolicy: successful preview completion resets seek state",
          "[decode_thread][exact_seek_publish]") {
    auto completion = complete_exact_seek_preview_publish(false);
    REQUIRE(completion.apply);
    REQUIRE(completion.output_state == TrackState::Ready);
    REQUIRE_FALSE(completion.pause_decode);
    REQUIRE_FALSE(completion.post_seek);
    REQUIRE(completion.exact_seek_target_us == -1);
    REQUIRE(completion.drain_decoder_before_next_packet);

    completion = complete_exact_seek_preview_publish(true);
    REQUIRE(completion.pause_decode);
}

TEST_CASE("ExactSeekPublishPolicy: completion plan gates failed preview publish",
          "[decode_thread][exact_seek_publish]") {
    auto completion = plan_exact_seek_preview_completion(
        true,
        false,
        true,
        1230000,
        3,
        2);
    REQUIRE(completion.apply);
    REQUIRE(completion.pause_decode);
    REQUIRE(completion.output_state == TrackState::Ready);
    REQUIRE_FALSE(completion.post_seek);
    REQUIRE(completion.exact_seek_target_us == -1);
    REQUIRE(completion.drain_decoder_before_next_packet);
    REQUIRE(completion.selected_pts_us == 1230000);
    REQUIRE(completion.published_count == 3);
    REQUIRE(completion.pending_count == 2);

    completion = plan_exact_seek_preview_completion(
        false,
        false,
        true,
        1230000,
        3,
        2);
    REQUIRE_FALSE(completion.apply);
    REQUIRE_FALSE(completion.pause_decode);
    REQUIRE(completion.published_count == 0);

    completion = plan_exact_seek_preview_completion(
        true,
        true,
        true,
        1230000,
        3,
        2);
    REQUIRE_FALSE(completion.apply);
}
