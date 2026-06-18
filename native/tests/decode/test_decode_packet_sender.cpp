#include <catch2/catch_test_macros.hpp>

#include "renderer/decode/codec_loop.h"
#include "renderer/decode/decode_packet_sender.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/error.h>
}

using namespace vr;

namespace {

AvPacketOwner make_packet() {
    auto packet = AvPacketOwner::allocate();
    REQUIRE(packet.get() != nullptr);
    return packet;
}

} // namespace

TEST_CASE("DecodePacketSender: sends and releases a packet",
          "[decode_thread][decode_packet_sender]") {
    auto packet = make_packet();
    int send_calls = 0;
    const auto result = send_decode_packet(
        packet,
        DecodePacketSendCallbacks{
            []() { return false; },
            []() { return false; },
            [&](AVPacket*) {
                ++send_calls;
                return 0;
            },
            {},
        });

    REQUIRE(result.sent);
    REQUIRE_FALSE(result.skipped);
    REQUIRE_FALSE(result.stop_with_error);
    REQUIRE(send_calls == 1);
    REQUIRE(packet.get() == nullptr);
}

TEST_CASE("DecodePacketSender: discards before decode without sending",
          "[decode_thread][decode_packet_sender]") {
    auto packet = make_packet();
    int send_calls = 0;
    const auto result = send_decode_packet(
        packet,
        DecodePacketSendCallbacks{
            []() { return true; },
            []() { return false; },
            [&](AVPacket*) {
                ++send_calls;
                return 0;
            },
            {},
        });

    REQUIRE(result.discarded_before_decode);
    REQUIRE_FALSE(result.sent);
    REQUIRE(send_calls == 0);
    REQUIRE(packet.get() == nullptr);
}

TEST_CASE("DecodePacketSender: aborts before send without sending",
          "[decode_thread][decode_packet_sender]") {
    auto packet = make_packet();
    int send_calls = 0;
    const auto result = send_decode_packet(
        packet,
        DecodePacketSendCallbacks{
            []() { return false; },
            []() { return true; },
            [&](AVPacket*) {
                ++send_calls;
                return 0;
            },
            {},
        });

    REQUIRE(result.aborted_before_send);
    REQUIRE_FALSE(result.sent);
    REQUIRE(send_calls == 0);
    REQUIRE(packet.get() == nullptr);
}

TEST_CASE("DecodePacketSender: reports fatal send errors",
          "[decode_thread][decode_packet_sender]") {
    auto packet = make_packet();
    const auto result = send_decode_packet(
        packet,
        DecodePacketSendCallbacks{
            []() { return false; },
            []() { return false; },
            [](AVPacket*) { return codec_loop_seh_caught_code(); },
            {},
        });

    REQUIRE(result.stop_with_error);
    REQUIRE_FALSE(result.sent);
    REQUIRE(packet.get() == nullptr);
}

TEST_CASE("DecodePacketSender: skips recoverable send errors",
          "[decode_thread][decode_packet_sender]") {
    auto packet = make_packet();
    int logged = 0;
    const auto result = send_decode_packet(
        packet,
        DecodePacketSendCallbacks{
            []() { return false; },
            []() { return false; },
            [](AVPacket*) { return AVERROR_INVALIDDATA; },
            [&](int) { ++logged; },
        });

    REQUIRE(result.skipped);
    REQUIRE_FALSE(result.sent);
    REQUIRE_FALSE(result.stop_with_error);
    REQUIRE(logged == 1);
    REQUIRE(packet.get() == nullptr);
}
