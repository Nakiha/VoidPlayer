#pragma once

#include "media/packet_queue.h"
#include "renderer/buffer/track_buffer.h"

#include <cstddef>

namespace vr {

bool should_publish_pending_exact_seek_frames(size_t pending_count,
                                              bool decode_paused,
                                              TrackState output_state);

bool should_drain_decoder_before_next_packet(bool drain_requested,
                                             bool decode_paused,
                                             TrackState output_state);

bool should_pause_decode_consumption(bool decode_paused,
                                     TrackState output_state);

bool should_discard_packet_before_decode(bool seek_pending,
                                         bool decode_paused,
                                         TrackState output_state);

bool should_abort_packet_before_send(bool cancelled);

enum class DecodePacketPopAction {
    ProcessPacket,
    SleepAndContinue,
    HandleQueueGapOrEof,
};

enum class DecodePacketSendAction {
    ReceiveFrames,
    SkipPacket,
    StopWithError,
};

enum class DecodeFrameReceiveAction {
    PublishFrame,
    Stop,
    StopWithLoggedError,
    StopWithError,
};

DecodePacketPopAction choose_decode_packet_pop_action(PacketPopStatus status,
                                                      bool packet_present,
                                                      bool running,
                                                      bool cancelled);

DecodePacketSendAction choose_decode_packet_send_action(int codec_ret);

bool should_stop_receive_loop_before_frame(bool cancelled);

DecodeFrameReceiveAction choose_decode_frame_receive_action(int codec_ret);

enum class EofDrainAction {
    None,
    BufferingExactSeekDrain,
    BufferingMarkFlushed,
    CodecDrain,
};

struct ExactSeekReorderPublishDecision {
    bool drain_codec = false;
    bool publish = false;
};

EofDrainAction choose_eof_drain_action(bool queue_eof,
                                       bool eof_flushed,
                                       TrackState output_state,
                                       bool exact_seek_active);

ExactSeekReorderPublishDecision choose_exact_seek_reorder_publish(
    bool exact_seek_active,
    size_t reorder_count,
    bool queue_eof,
    size_t queue_size,
    bool eof_flushed,
    bool preview_window_ready);

bool should_publish_exact_seek_preview_after_collect(
    bool exact_seek_active,
    bool preview_window_ready);

bool should_pace_hevc_hardware_exact_seek_decode(
    bool exact_seek_active,
    bool hevc_hardware_decode);

bool should_complete_buffering_eof_preroll(size_t buffered_frames);

} // namespace vr
