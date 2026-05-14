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

} // namespace vr
