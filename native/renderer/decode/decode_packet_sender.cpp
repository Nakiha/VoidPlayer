#include "renderer/decode/decode_packet_sender.h"

#include "renderer/decode/decode_loop_policy.h"

extern "C" {
#include <libavcodec/packet.h>
}

namespace vr {

DecodePacketSendResult send_decode_packet(
    AVPacket*& packet,
    const DecodePacketSendCallbacks& callbacks) {
    DecodePacketSendResult result;
    if (!packet) {
        result.skipped = true;
        return result;
    }

    if (callbacks.should_discard_before_decode &&
        callbacks.should_discard_before_decode()) {
        av_packet_free(&packet);
        result.discarded_before_decode = true;
        return result;
    }

    if (callbacks.should_abort_before_send &&
        callbacks.should_abort_before_send()) {
        av_packet_free(&packet);
        result.aborted_before_send = true;
        return result;
    }

    const int ret = callbacks.send_packet ? callbacks.send_packet(packet) : -1;
    const auto send_action = choose_decode_packet_send_action(ret);
    av_packet_free(&packet);

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
