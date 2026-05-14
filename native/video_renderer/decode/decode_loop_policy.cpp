#include "video_renderer/decode/decode_loop_policy.h"

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

} // namespace vr
