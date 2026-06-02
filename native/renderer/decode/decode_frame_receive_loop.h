#pragma once

#include <cstdint>
#include <functional>

struct AVFrame;

namespace vr {

struct DecodeFrameReceiveLoopOptions {
    bool exact_seek_active = false;
    int64_t exact_seek_target_us = -1;
    bool had_decoded_frames_before_loop = false;
};

struct DecodeFrameReceiveLoopCallbacks {
    std::function<bool()> should_stop_before_receive;
    std::function<int(AVFrame*)> receive_frame;
    std::function<void(AVFrame*)> rescale_timestamps;
    std::function<void(const AVFrame*)> on_frame_ready;
    std::function<void()> on_exact_seek_frame_dropped;
    std::function<void(AVFrame*)> collect_exact_seek_candidate;
    std::function<bool()> exact_seek_preview_window_ready;
    std::function<void()> publish_best_exact_seek_frame;
    std::function<void()> mark_first_visible_frame_pending;
    std::function<bool(AVFrame*)> publish_frame;
    std::function<void()> complete_preroll_if_ready;
    std::function<void(int)> log_receive_error;
};

struct DecodeFrameReceiveLoopResult {
    bool stop_with_error = false;
    int frames_produced = 0;
};

DecodeFrameReceiveLoopResult receive_decode_frames_for_packet(
    AVFrame* frame,
    const DecodeFrameReceiveLoopOptions& options,
    const DecodeFrameReceiveLoopCallbacks& callbacks);

} // namespace vr
