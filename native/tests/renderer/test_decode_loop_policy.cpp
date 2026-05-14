#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/decode_loop_policy.h"

using namespace vr;

TEST_CASE("DecodeLoopPolicy: pending exact-seek frames publish only when unpaused",
          "[decode_thread][decode_loop_policy]") {
    REQUIRE_FALSE(should_publish_pending_exact_seek_frames(0, false, TrackState::Ready));
    REQUIRE_FALSE(should_publish_pending_exact_seek_frames(1, true, TrackState::Ready));
    REQUIRE_FALSE(should_publish_pending_exact_seek_frames(1, false, TrackState::Flushing));
    REQUIRE(should_publish_pending_exact_seek_frames(1, false, TrackState::Buffering));
}

TEST_CASE("DecodeLoopPolicy: drain-before-next-packet is gated by pause and flushing",
          "[decode_thread][decode_loop_policy]") {
    REQUIRE_FALSE(should_drain_decoder_before_next_packet(false, false, TrackState::Ready));
    REQUIRE_FALSE(should_drain_decoder_before_next_packet(true, true, TrackState::Ready));
    REQUIRE_FALSE(should_drain_decoder_before_next_packet(true, false, TrackState::Flushing));
    REQUIRE(should_drain_decoder_before_next_packet(true, false, TrackState::Ready));
}

TEST_CASE("DecodeLoopPolicy: paused decode preserves queued packets unless flushing",
          "[decode_thread][decode_loop_policy]") {
    REQUIRE_FALSE(should_pause_decode_consumption(false, TrackState::Ready));
    REQUIRE_FALSE(should_pause_decode_consumption(true, TrackState::Flushing));
    REQUIRE(should_pause_decode_consumption(true, TrackState::Buffering));
}

TEST_CASE("DecodeLoopPolicy: packets are discarded before codec during stale epochs",
          "[decode_thread][decode_loop_policy]") {
    REQUIRE_FALSE(should_discard_packet_before_decode(false, false, TrackState::Ready));
    REQUIRE(should_discard_packet_before_decode(true, false, TrackState::Ready));
    REQUIRE(should_discard_packet_before_decode(false, true, TrackState::Ready));
    REQUIRE(should_discard_packet_before_decode(false, false, TrackState::Flushing));
}
