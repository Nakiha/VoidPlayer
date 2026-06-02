#pragma once

#include "renderer/buffer/track_buffer.h"

namespace vr {

enum class DecodeDrainReceiveAction {
    PublishFrame,
    Stop,
    StopAndClearDrainRequest,
    StopWithError,
    StopWithErrorAndClearDrainRequest,
};

enum class EofCodecSendAction {
    ReceiveFrames,
    Stop,
    StopWithError,
};

bool should_abort_drain_before_receive(bool cancelled, TrackState output_state);
bool should_stop_drain_after_publish(bool decode_paused, TrackState output_state);

DecodeDrainReceiveAction choose_drain_before_next_packet_receive_action(int codec_ret);
EofCodecSendAction choose_eof_codec_send_action(int send_ret);
DecodeDrainReceiveAction choose_eof_codec_receive_action(int codec_ret);

} // namespace vr
