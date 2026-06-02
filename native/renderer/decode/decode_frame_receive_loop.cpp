#include "renderer/decode/decode_frame_receive_loop.h"

#include "renderer/decode/av_frame_lifetime.h"
#include "renderer/decode/decode_loop_policy.h"
#include "renderer/decode/exact_seek_window.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace vr {

DecodeFrameReceiveLoopResult receive_decode_frames_for_packet(
    AVFrame* frame,
    const DecodeFrameReceiveLoopOptions& options,
    const DecodeFrameReceiveLoopCallbacks& callbacks) {
    DecodeFrameReceiveLoopResult result;
    while (true) {
        if (callbacks.should_stop_before_receive &&
            callbacks.should_stop_before_receive()) {
            break;
        }

        const int ret = callbacks.receive_frame ? callbacks.receive_frame(frame) : -1;
        const auto receive_action = choose_decode_frame_receive_action(ret);
        if (receive_action == DecodeFrameReceiveAction::StopWithError) {
            result.stop_with_error = true;
            break;
        }
        if (receive_action == DecodeFrameReceiveAction::Stop) {
            break;
        }
        if (receive_action == DecodeFrameReceiveAction::StopWithLoggedError) {
            if (callbacks.log_receive_error) {
                callbacks.log_receive_error(ret);
            }
            break;
        }

        AvFrameUnrefGuard frame_guard(frame);
        if (callbacks.rescale_timestamps) {
            callbacks.rescale_timestamps(frame);
        }
        if (callbacks.on_frame_ready) {
            callbacks.on_frame_ready(frame);
        }

        if (options.exact_seek_active) {
            const int64_t pts_us = frame ? frame->pts : -1;
            if (!should_collect_exact_seek_candidate(
                    pts_us, options.exact_seek_target_us)) {
                if (callbacks.on_exact_seek_frame_dropped) {
                    callbacks.on_exact_seek_frame_dropped();
                }
                continue;
            }

            ++result.frames_produced;
            if (callbacks.collect_exact_seek_candidate) {
                callbacks.collect_exact_seek_candidate(frame);
            }

            const bool preview_ready =
                callbacks.exact_seek_preview_window_ready &&
                callbacks.exact_seek_preview_window_ready();
            if (should_publish_exact_seek_preview_after_collect(
                    options.exact_seek_active,
                    preview_ready)) {
                if (callbacks.publish_best_exact_seek_frame) {
                    callbacks.publish_best_exact_seek_frame();
                }
                break;
            }

            continue;
        }

        ++result.frames_produced;
        if (result.frames_produced == 1 &&
            !options.had_decoded_frames_before_loop &&
            callbacks.mark_first_visible_frame_pending) {
            callbacks.mark_first_visible_frame_pending();
        }

        if (callbacks.publish_frame && !callbacks.publish_frame(frame)) {
            break;
        }
        if (callbacks.complete_preroll_if_ready) {
            callbacks.complete_preroll_if_ready();
        }
    }
    return result;
}

} // namespace vr
