#include "video_renderer/decode/decode_seek_epoch.h"

namespace vr {

std::optional<DecodeSeekNotification> take_pending_decode_seek(DecodePendingSeekState& state) {
    if (!state.pending) {
        return std::nullopt;
    }
    state.pending = false;
    if (state.target_pts_us < 0) {
        return std::nullopt;
    }
    return DecodeSeekNotification{
        state.target_pts_us,
        state.type,
    };
}

DecodeSeekEpochStartState build_decode_seek_epoch_start_state(
    const DecodeSeekNotification& notification,
    bool hw_enabled) {
    DecodeSeekEpochStartState state;
    state.exact_seek_target_us =
        notification.type == SeekType::Exact ? notification.target_pts_us : -1;
    state.hw_visibility_flush_pending = hw_enabled;
    return state;
}

const char* decode_seek_type_name(SeekType type) {
    return type == SeekType::Exact ? "Exact" : "Keyframe";
}

} // namespace vr
