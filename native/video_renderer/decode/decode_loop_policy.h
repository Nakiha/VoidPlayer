#pragma once

#include "video_renderer/buffer/track_buffer.h"

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

} // namespace vr
