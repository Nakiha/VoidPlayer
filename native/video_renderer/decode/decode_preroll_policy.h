#pragma once

#include "video_renderer/buffer/track_buffer.h"

#include <cstddef>

namespace vr {

struct DecodePrerollTransitionDecision {
    bool complete = false;
    TrackState output_state = TrackState::Buffering;
    bool pause_decode = false;
    bool clear_post_seek = false;
};

size_t decode_post_seek_preroll_target(bool hw_enabled);

bool is_decode_preroll_ready(bool post_seek,
                             bool hw_enabled,
                             size_t buffered_frames,
                             bool full_preroll_ready);

DecodePrerollTransitionDecision choose_decode_preroll_transition(
    TrackState output_state,
    bool preroll_ready,
    bool pause_after_preroll);

} // namespace vr
