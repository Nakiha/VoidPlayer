#include "video_renderer/decode/decode_preroll_policy.h"

namespace vr {

size_t decode_post_seek_preroll_target(bool hw_enabled) {
    // Hardware-decoded seek/add-track previews are more stable if we wait for
    // one extra frame before exposing the paused preview. Some GPU/driver
    // combinations can produce a partially-ready first post-seek frame.
    return hw_enabled ? size_t(2) : size_t(1);
}

bool is_decode_preroll_ready(bool post_seek,
                             bool hw_enabled,
                             size_t buffered_frames,
                             bool full_preroll_ready) {
    if (post_seek) {
        return buffered_frames >= decode_post_seek_preroll_target(hw_enabled);
    }
    return full_preroll_ready;
}

DecodePrerollTransitionDecision choose_decode_preroll_transition(
    TrackState output_state,
    bool preroll_ready,
    bool pause_after_preroll) {
    DecodePrerollTransitionDecision decision;
    decision.output_state = output_state;
    if (output_state != TrackState::Buffering || !preroll_ready) {
        return decision;
    }
    decision.complete = true;
    decision.output_state = TrackState::Ready;
    decision.pause_decode = pause_after_preroll;
    decision.clear_post_seek = true;
    return decision;
}

} // namespace vr
