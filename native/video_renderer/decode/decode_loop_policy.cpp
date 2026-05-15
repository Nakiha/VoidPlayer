#include "video_renderer/decode/decode_loop_policy.h"

#include "video_renderer/decode/codec_loop.h"

namespace vr {
namespace {

bool is_flushing(TrackState state) {
    return state == TrackState::Flushing;
}

} // namespace

bool should_publish_pending_exact_seek_frames(size_t pending_count,
                                              bool decode_paused,
                                              TrackState output_state) {
    return pending_count > 0 && !decode_paused && !is_flushing(output_state);
}

bool should_drain_decoder_before_next_packet(bool drain_requested,
                                             bool decode_paused,
                                             TrackState output_state) {
    return drain_requested && !decode_paused && !is_flushing(output_state);
}

bool should_pause_decode_consumption(bool decode_paused,
                                     TrackState output_state) {
    return decode_paused && !is_flushing(output_state);
}

bool should_discard_packet_before_decode(bool seek_pending,
                                         bool decode_paused,
                                         TrackState output_state) {
    return seek_pending || decode_paused || is_flushing(output_state);
}

bool should_abort_packet_before_send(bool cancelled) {
    return cancelled;
}

DecodePacketPopAction choose_decode_packet_pop_action(PacketPopStatus status,
                                                      bool packet_present,
                                                      bool running,
                                                      bool cancelled) {
    if (status == PacketPopStatus::Packet && packet_present) {
        return DecodePacketPopAction::ProcessPacket;
    }
    if (!running || cancelled) {
        return DecodePacketPopAction::SleepAndContinue;
    }
    return DecodePacketPopAction::HandleQueueGapOrEof;
}

DecodePacketSendAction choose_decode_packet_send_action(int codec_ret) {
    if (codec_loop_is_seh_caught(codec_ret)) {
        return DecodePacketSendAction::StopWithError;
    }
    if (codec_ret < 0 && !codec_loop_is_again_or_eof(codec_ret)) {
        return DecodePacketSendAction::SkipPacket;
    }
    return DecodePacketSendAction::ReceiveFrames;
}

bool should_stop_receive_loop_before_frame(bool cancelled) {
    return cancelled;
}

DecodeFrameReceiveAction choose_decode_frame_receive_action(int codec_ret) {
    if (codec_loop_is_seh_caught(codec_ret)) {
        return DecodeFrameReceiveAction::StopWithError;
    }
    if (codec_loop_is_again_or_eof(codec_ret)) {
        return DecodeFrameReceiveAction::Stop;
    }
    if (codec_ret < 0) {
        return DecodeFrameReceiveAction::StopWithLoggedError;
    }
    return DecodeFrameReceiveAction::PublishFrame;
}

EofDrainAction choose_eof_drain_action(bool queue_eof,
                                       bool eof_flushed,
                                       TrackState output_state,
                                       bool exact_seek_active) {
    if (!queue_eof || eof_flushed) {
        return EofDrainAction::None;
    }
    if (output_state == TrackState::Buffering) {
        return exact_seek_active
            ? EofDrainAction::BufferingExactSeekDrain
            : EofDrainAction::BufferingMarkFlushed;
    }
    return EofDrainAction::CodecDrain;
}

ExactSeekReorderPublishDecision choose_exact_seek_reorder_publish(
    bool exact_seek_active,
    size_t reorder_count,
    bool queue_eof,
    size_t queue_size,
    bool eof_flushed,
    bool preview_window_ready) {
    ExactSeekReorderPublishDecision decision;
    if (!exact_seek_active || reorder_count == 0) {
        return decision;
    }
    if (queue_eof && queue_size == 0) {
        decision.drain_codec = !eof_flushed;
        decision.publish = true;
        return decision;
    }
    decision.publish = preview_window_ready;
    return decision;
}

bool should_publish_exact_seek_preview_after_collect(
    bool exact_seek_active,
    bool preview_window_ready) {
    return exact_seek_active && preview_window_ready;
}

bool should_pace_hardware_exact_seek_decode(
    bool exact_seek_active,
    bool hardware_decode) {
    return exact_seek_active && hardware_decode;
}

} // namespace vr
