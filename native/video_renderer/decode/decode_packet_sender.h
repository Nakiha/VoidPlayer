#pragma once

#include <functional>

struct AVPacket;

namespace vr {

struct DecodePacketSendCallbacks {
    std::function<bool()> should_discard_before_decode;
    std::function<bool()> should_abort_before_send;
    std::function<int(AVPacket*)> send_packet;
    std::function<void(int)> log_send_error;
};

struct DecodePacketSendResult {
    bool sent = false;
    bool skipped = false;
    bool stop_with_error = false;
    bool discarded_before_decode = false;
    bool aborted_before_send = false;
};

DecodePacketSendResult send_decode_packet(
    AVPacket*& packet,
    const DecodePacketSendCallbacks& callbacks);

} // namespace vr
