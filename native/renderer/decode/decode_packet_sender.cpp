#include "renderer/decode/decode_packet_sender.h"

#include "renderer/decode/decode_loop_policy.h"

namespace vr {

DecodePacketSendResult send_decode_packet(
    AvPacketOwner& packet,
    const DecodePacketSendCallbacks& callbacks) {
    DecodePacketSendResult result;
    if (!packet) {
        result.skipped = true;
        return result;
    }

    if (callbacks.should_discard_before_decode &&
        callbacks.should_discard_before_decode()) {
        packet.reset();
        result.discarded_before_decode = true;
        return result;
    }

    if (callbacks.should_abort_before_send &&
        callbacks.should_abort_before_send()) {
        packet.reset();
        result.aborted_before_send = true;
        return result;
    }

    const int ret = callbacks.send_packet ? callbacks.send_packet(packet.get()) : -1;
    const auto send_action = choose_decode_packet_send_action(ret);
    packet.reset();

    if (send_action == DecodePacketSendAction::StopWithError) {
        result.stop_with_error = true;
        return result;
    }

    if (send_action == DecodePacketSendAction::SkipPacket) {
        result.skipped = true;
        if (callbacks.log_send_error) {
            callbacks.log_send_error(ret);
        }
        return result;
    }

    result.sent = true;
    return result;
}

} // namespace vr
