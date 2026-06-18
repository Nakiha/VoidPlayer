#include <catch2/catch_test_macros.hpp>

#include "renderer/decode/decode_seek_epoch.h"

#include <string>

using namespace vr;

TEST_CASE("DecodeSeekEpoch: taking a pending seek clears only the pending flag",
          "[decode_thread][decode_seek_epoch]") {
    DecodePendingSeekState state;
    state.pending = true;
    state.target_pts_us = 1234567;
    state.type = SeekType::Exact;

    const auto notification = take_pending_decode_seek(state);

    REQUIRE(notification.has_value());
    REQUIRE(notification->target_pts_us == 1234567);
    REQUIRE(notification->type == SeekType::Exact);
    REQUIRE_FALSE(state.pending);
    REQUIRE(state.target_pts_us == 1234567);
    REQUIRE(state.type == SeekType::Exact);
}

TEST_CASE("DecodeSeekEpoch: taking an idle seek state returns no notification",
          "[decode_thread][decode_seek_epoch]") {
    DecodePendingSeekState state;
    state.target_pts_us = 42000;
    state.type = SeekType::Keyframe;

    const auto notification = take_pending_decode_seek(state);

    REQUIRE_FALSE(notification.has_value());
    REQUIRE_FALSE(state.pending);
    REQUIRE(state.target_pts_us == 42000);
    REQUIRE(state.type == SeekType::Keyframe);
}

TEST_CASE("DecodeSeekEpoch: taking an invalid target still consumes the pending flag",
          "[decode_thread][decode_seek_epoch]") {
    DecodePendingSeekState state;
    state.pending = true;
    state.target_pts_us = -1;
    state.type = SeekType::Exact;

    const auto notification = take_pending_decode_seek(state);

    REQUIRE_FALSE(notification.has_value());
    REQUIRE_FALSE(state.pending);
    REQUIRE(state.target_pts_us == -1);
    REQUIRE(state.type == SeekType::Exact);
}

TEST_CASE("DecodeSeekEpoch: exact seek start state preserves target and buffers",
          "[decode_thread][decode_seek_epoch]") {
    const auto state = build_decode_seek_epoch_start_state(
        DecodeSeekNotification{9000000, SeekType::Exact},
        true);

    REQUIRE(state.exact_seek_target_us == 9000000);
    REQUIRE(state.post_seek);
    REQUIRE(state.hw_visibility_flush_pending);
    REQUIRE_FALSE(state.eof_flushed);
    REQUIRE_FALSE(state.decode_paused);
    REQUIRE(state.output_state == TrackState::Buffering);
}

TEST_CASE("DecodeSeekEpoch: keyframe seek disables exact target and mirrors hw flush",
          "[decode_thread][decode_seek_epoch]") {
    const auto state = build_decode_seek_epoch_start_state(
        DecodeSeekNotification{9000000, SeekType::Keyframe},
        false);

    REQUIRE(state.exact_seek_target_us == -1);
    REQUIRE(state.post_seek);
    REQUIRE_FALSE(state.hw_visibility_flush_pending);
    REQUIRE_FALSE(state.eof_flushed);
    REQUIRE_FALSE(state.decode_paused);
    REQUIRE(state.output_state == TrackState::Buffering);
    REQUIRE(decode_seek_type_name(SeekType::Exact) == std::string("Exact"));
    REQUIRE(decode_seek_type_name(SeekType::Keyframe) == std::string("Keyframe"));
}
