#include "renderer/decode/decode_drain_policy.h"

#include "renderer/decode/codec_loop.h"

namespace vr {

bool should_abort_drain_before_receive(bool cancelled, TrackState output_state) {
    return cancelled || output_state == TrackState::Flushing;
}

bool should_stop_drain_after_publish(bool decode_paused, TrackState output_state) {
    return decode_paused || output_state == TrackState::Flushing;
}

DecodeDrainReceiveAction choose_drain_before_next_packet_receive_action(int codec_ret) {
    if (codec_loop_is_seh_caught(codec_ret)) {
        return DecodeDrainReceiveAction::StopWithErrorAndClearDrainRequest;
    }
    if (codec_loop_is_again_or_eof(codec_ret) || codec_ret < 0) {
        return DecodeDrainReceiveAction::StopAndClearDrainRequest;
    }
    return DecodeDrainReceiveAction::PublishFrame;
}

EofCodecSendAction choose_eof_codec_send_action(int send_ret) {
    if (send_ret >= 0 || codec_loop_is_again_or_eof(send_ret)) {
        return EofCodecSendAction::ReceiveFrames;
    }
    if (codec_loop_is_seh_caught(send_ret)) {
        return EofCodecSendAction::StopWithError;
    }
    return EofCodecSendAction::Stop;
}

DecodeDrainReceiveAction choose_eof_codec_receive_action(int codec_ret) {
    if (codec_loop_is_seh_caught(codec_ret)) {
        return DecodeDrainReceiveAction::StopWithError;
    }
    if (codec_loop_is_again_or_eof(codec_ret) || codec_ret < 0) {
        return DecodeDrainReceiveAction::Stop;
    }
    return DecodeDrainReceiveAction::PublishFrame;
}

} // namespace vr
