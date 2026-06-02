#include <catch2/catch_test_macros.hpp>

#include "renderer/decode/codec_loop.h"
#include "renderer/decode/decode_loop_policy.h"

extern "C" {
#include <libavutil/error.h>
}

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

TEST_CASE("DecodeLoopPolicy: packet pop action preserves queue gap handling",
          "[decode_thread][decode_loop_policy]") {
    REQUIRE(choose_decode_packet_pop_action(
                PacketPopStatus::Packet, true, true, false) ==
            DecodePacketPopAction::ProcessPacket);
    REQUIRE(choose_decode_packet_pop_action(
                PacketPopStatus::Packet, true, false, true) ==
            DecodePacketPopAction::ProcessPacket);
    REQUIRE(choose_decode_packet_pop_action(
                PacketPopStatus::Packet, false, true, false) ==
            DecodePacketPopAction::HandleQueueGapOrEof);
    REQUIRE(choose_decode_packet_pop_action(
                PacketPopStatus::Empty, false, false, false) ==
            DecodePacketPopAction::SleepAndContinue);
    REQUIRE(choose_decode_packet_pop_action(
                PacketPopStatus::Eof, false, true, true) ==
            DecodePacketPopAction::SleepAndContinue);
    REQUIRE(choose_decode_packet_pop_action(
                PacketPopStatus::Eof, false, true, false) ==
            DecodePacketPopAction::HandleQueueGapOrEof);
}

TEST_CASE("DecodeLoopPolicy: packet send action keeps receive loop contract",
          "[decode_thread][decode_loop_policy]") {
    REQUIRE(choose_decode_packet_send_action(0) ==
            DecodePacketSendAction::ReceiveFrames);
    REQUIRE(choose_decode_packet_send_action(AVERROR(EAGAIN)) ==
            DecodePacketSendAction::ReceiveFrames);
    REQUIRE(choose_decode_packet_send_action(AVERROR_EOF) ==
            DecodePacketSendAction::ReceiveFrames);
    REQUIRE(choose_decode_packet_send_action(AVERROR_INVALIDDATA) ==
            DecodePacketSendAction::SkipPacket);
    REQUIRE(choose_decode_packet_send_action(codec_loop_seh_caught_code()) ==
            DecodePacketSendAction::StopWithError);

    REQUIRE_FALSE(should_abort_packet_before_send(false));
    REQUIRE(should_abort_packet_before_send(true));
}

TEST_CASE("DecodeLoopPolicy: frame receive action separates stop and error paths",
          "[decode_thread][decode_loop_policy]") {
    REQUIRE(choose_decode_frame_receive_action(0) ==
            DecodeFrameReceiveAction::PublishFrame);
    REQUIRE(choose_decode_frame_receive_action(AVERROR(EAGAIN)) ==
            DecodeFrameReceiveAction::Stop);
    REQUIRE(choose_decode_frame_receive_action(AVERROR_EOF) ==
            DecodeFrameReceiveAction::Stop);
    REQUIRE(choose_decode_frame_receive_action(AVERROR_INVALIDDATA) ==
            DecodeFrameReceiveAction::StopWithLoggedError);
    REQUIRE(choose_decode_frame_receive_action(codec_loop_seh_caught_code()) ==
            DecodeFrameReceiveAction::StopWithError);

    REQUIRE_FALSE(should_stop_receive_loop_before_frame(false));
    REQUIRE(should_stop_receive_loop_before_frame(true));
}

TEST_CASE("DecodeLoopPolicy: EOF drain action preserves buffering semantics",
          "[decode_thread][decode_loop_policy]") {
    REQUIRE(choose_eof_drain_action(false, false, TrackState::Ready, false) ==
            EofDrainAction::None);
    REQUIRE(choose_eof_drain_action(true, true, TrackState::Ready, false) ==
            EofDrainAction::None);
    REQUIRE(choose_eof_drain_action(true, false, TrackState::Buffering, true) ==
            EofDrainAction::BufferingExactSeekDrain);
    REQUIRE(choose_eof_drain_action(true, false, TrackState::Buffering, false) ==
            EofDrainAction::BufferingMarkFlushed);
    REQUIRE(choose_eof_drain_action(true, false, TrackState::Ready, false) ==
            EofDrainAction::CodecDrain);
}

TEST_CASE("DecodeLoopPolicy: exact-seek reorder publishes on preview window or EOF",
          "[decode_thread][decode_loop_policy]") {
    auto decision = choose_exact_seek_reorder_publish(
        false, 3, true, 0, false, true);
    REQUIRE_FALSE(decision.drain_codec);
    REQUIRE_FALSE(decision.publish);

    decision = choose_exact_seek_reorder_publish(
        true, 0, true, 0, false, true);
    REQUIRE_FALSE(decision.drain_codec);
    REQUIRE_FALSE(decision.publish);

    decision = choose_exact_seek_reorder_publish(
        true, 2, false, 4, false, true);
    REQUIRE_FALSE(decision.drain_codec);
    REQUIRE(decision.publish);

    decision = choose_exact_seek_reorder_publish(
        true, 2, true, 0, false, false);
    REQUIRE(decision.drain_codec);
    REQUIRE(decision.publish);

    decision = choose_exact_seek_reorder_publish(
        true, 2, true, 0, true, false);
    REQUIRE_FALSE(decision.drain_codec);
    REQUIRE(decision.publish);
}

TEST_CASE("DecodeLoopPolicy: exact-seek preview publish and pacing gates stay explicit",
          "[decode_thread][decode_loop_policy]") {
    REQUIRE_FALSE(should_publish_exact_seek_preview_after_collect(false, true));
    REQUIRE_FALSE(should_publish_exact_seek_preview_after_collect(true, false));
    REQUIRE(should_publish_exact_seek_preview_after_collect(true, true));

    REQUIRE_FALSE(should_pace_hardware_exact_seek_decode(false, true));
    REQUIRE_FALSE(should_pace_hardware_exact_seek_decode(true, false));
    REQUIRE(should_pace_hardware_exact_seek_decode(true, true));
}
