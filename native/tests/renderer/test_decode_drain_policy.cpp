#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/codec_loop.h"
#include "video_renderer/decode/decode_drain_policy.h"

extern "C" {
#include <libavutil/error.h>
}

using namespace vr;

TEST_CASE("DecodeDrainPolicy: drain-before-next-packet receive actions",
          "[decode_thread][decode_drain_policy]") {
    REQUIRE(choose_drain_before_next_packet_receive_action(0) ==
            DecodeDrainReceiveAction::PublishFrame);
    REQUIRE(choose_drain_before_next_packet_receive_action(AVERROR(EAGAIN)) ==
            DecodeDrainReceiveAction::StopAndClearDrainRequest);
    REQUIRE(choose_drain_before_next_packet_receive_action(AVERROR_EOF) ==
            DecodeDrainReceiveAction::StopAndClearDrainRequest);
    REQUIRE(choose_drain_before_next_packet_receive_action(AVERROR_INVALIDDATA) ==
            DecodeDrainReceiveAction::StopAndClearDrainRequest);
    REQUIRE(choose_drain_before_next_packet_receive_action(codec_loop_seh_caught_code()) ==
            DecodeDrainReceiveAction::StopWithErrorAndClearDrainRequest);
}

TEST_CASE("DecodeDrainPolicy: EOF codec send action preserves drain behavior",
          "[decode_thread][decode_drain_policy]") {
    REQUIRE(choose_eof_codec_send_action(0) == EofCodecSendAction::ReceiveFrames);
    REQUIRE(choose_eof_codec_send_action(AVERROR(EAGAIN)) ==
            EofCodecSendAction::ReceiveFrames);
    REQUIRE(choose_eof_codec_send_action(AVERROR_EOF) ==
            EofCodecSendAction::ReceiveFrames);
    REQUIRE(choose_eof_codec_send_action(AVERROR_INVALIDDATA) ==
            EofCodecSendAction::Stop);
    REQUIRE(choose_eof_codec_send_action(codec_loop_seh_caught_code()) ==
            EofCodecSendAction::StopWithError);
}

TEST_CASE("DecodeDrainPolicy: EOF receive action stops or publishes explicitly",
          "[decode_thread][decode_drain_policy]") {
    REQUIRE(choose_eof_codec_receive_action(0) ==
            DecodeDrainReceiveAction::PublishFrame);
    REQUIRE(choose_eof_codec_receive_action(AVERROR(EAGAIN)) ==
            DecodeDrainReceiveAction::Stop);
    REQUIRE(choose_eof_codec_receive_action(AVERROR_EOF) ==
            DecodeDrainReceiveAction::Stop);
    REQUIRE(choose_eof_codec_receive_action(AVERROR_INVALIDDATA) ==
            DecodeDrainReceiveAction::Stop);
    REQUIRE(choose_eof_codec_receive_action(codec_loop_seh_caught_code()) ==
            DecodeDrainReceiveAction::StopWithError);
}

TEST_CASE("DecodeDrainPolicy: drain loop stop gates match decode state",
          "[decode_thread][decode_drain_policy]") {
    REQUIRE_FALSE(should_abort_drain_before_receive(false, TrackState::Ready));
    REQUIRE(should_abort_drain_before_receive(true, TrackState::Ready));
    REQUIRE(should_abort_drain_before_receive(false, TrackState::Flushing));

    REQUIRE_FALSE(should_stop_drain_after_publish(false, TrackState::Ready));
    REQUIRE(should_stop_drain_after_publish(true, TrackState::Ready));
    REQUIRE(should_stop_drain_after_publish(false, TrackState::Flushing));
}
