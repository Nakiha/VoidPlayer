#pragma once

#include "media/seek_controller.h"
#include "renderer/buffer/track_buffer.h"

#include <cstdint>
#include <optional>

namespace vr {

struct DecodePendingSeekState {
    bool pending = false;
    int64_t target_pts_us = 0;
    SeekType type = SeekType::Keyframe;
};

struct DecodeSeekNotification {
    int64_t target_pts_us = -1;
    SeekType type = SeekType::Keyframe;
};

struct DecodeSeekEpochStartState {
    int64_t exact_seek_target_us = -1;
    bool post_seek = true;
    bool hw_visibility_flush_pending = false;
    bool eof_flushed = false;
    bool decode_paused = false;
    TrackState output_state = TrackState::Buffering;
};

std::optional<DecodeSeekNotification> take_pending_decode_seek(DecodePendingSeekState& state);

DecodeSeekEpochStartState build_decode_seek_epoch_start_state(
    const DecodeSeekNotification& notification,
    bool hw_enabled);

const char* decode_seek_type_name(SeekType type);

} // namespace vr
