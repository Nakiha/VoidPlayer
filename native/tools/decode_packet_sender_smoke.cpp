#include "renderer/decode/codec_loop.h"
#include "renderer/decode/decode_packet_sender.h"

#include <iostream>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/error.h>
}

namespace {

int fail(const char* message) {
    std::cerr << message << "\n";
    return 1;
}

vr::AvPacketOwner make_packet() {
    return vr::AvPacketOwner::allocate();
}

} // namespace

int main() {
    auto sent_packet = make_packet();
    if (!sent_packet) {
        return fail("failed to allocate sent packet");
    }
    int send_calls = 0;
    const auto sent = vr::send_decode_packet(
        sent_packet,
        vr::DecodePacketSendCallbacks{
            []() { return false; },
            []() { return false; },
            [&](AVPacket*) {
                ++send_calls;
                return 0;
            },
            {},
        });
    if (!sent.sent || sent_packet || send_calls != 1) {
        return fail("decode packet sender did not send and release packet");
    }

    auto discarded_packet = make_packet();
    if (!discarded_packet) {
        return fail("failed to allocate discarded packet");
    }
    const auto discarded = vr::send_decode_packet(
        discarded_packet,
        vr::DecodePacketSendCallbacks{
            []() { return true; },
            []() { return false; },
            [](AVPacket*) { return 0; },
            {},
        });
    if (!discarded.discarded_before_decode || discarded_packet) {
        return fail("decode packet sender did not discard packet");
    }

    auto fatal_packet = make_packet();
    if (!fatal_packet) {
        return fail("failed to allocate fatal packet");
    }
    const auto fatal = vr::send_decode_packet(
        fatal_packet,
        vr::DecodePacketSendCallbacks{
            []() { return false; },
            []() { return false; },
            [](AVPacket*) { return vr::codec_loop_seh_caught_code(); },
            {},
        });
    if (!fatal.stop_with_error || fatal_packet) {
        return fail("decode packet sender did not report fatal send");
    }

    auto skipped_packet = make_packet();
    if (!skipped_packet) {
        return fail("failed to allocate skipped packet");
    }
    int logged = 0;
    const auto skipped = vr::send_decode_packet(
        skipped_packet,
        vr::DecodePacketSendCallbacks{
            []() { return false; },
            []() { return false; },
            [](AVPacket*) { return AVERROR_INVALIDDATA; },
            [&](int) { ++logged; },
        });
    if (!skipped.skipped || skipped.stop_with_error || skipped_packet || logged != 1) {
        return fail("decode packet sender did not skip recoverable send error");
    }

    return 0;
}
