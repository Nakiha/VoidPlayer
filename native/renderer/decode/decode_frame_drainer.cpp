#include "renderer/decode/decode_frame_drainer.h"

#include "renderer/decode/av_frame_lifetime.h"
#include "renderer/decode/decode_drain_policy.h"

namespace vr {

DecodeFrameDrainResult drain_frames_before_next_packet(
    AVFrame* frame,
    const DecodeFrameDrainCallbacks& callbacks) {
    DecodeFrameDrainResult result;
    while (true) {
        if (callbacks.should_abort_before_receive &&
            callbacks.should_abort_before_receive()) {
            result.clear_drain_request = true;
            break;
        }

        const int ret = callbacks.receive_frame ? callbacks.receive_frame(frame) : -1;
        const auto drain_action = choose_drain_before_next_packet_receive_action(ret);
        if (drain_action == DecodeDrainReceiveAction::StopWithErrorAndClearDrainRequest) {
            result.stop_with_error = true;
            result.clear_drain_request = true;
            break;
        }
        if (drain_action == DecodeDrainReceiveAction::StopAndClearDrainRequest) {
            result.clear_drain_request = true;
            break;
        }

        AvFrameUnrefGuard frame_guard(frame);
        if (callbacks.rescale_timestamps) {
            callbacks.rescale_timestamps(frame);
        }
        if (callbacks.on_frame_ready) {
            callbacks.on_frame_ready(frame);
        }
        if (callbacks.publish_frame && !callbacks.publish_frame(frame)) {
            break;
        }
        ++result.frames_published;

        if (callbacks.should_stop_after_publish &&
            callbacks.should_stop_after_publish()) {
            break;
        }
    }
    return result;
}

} // namespace vr
