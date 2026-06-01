#pragma once

#include <functional>

struct AVFrame;

namespace vr {

struct DecodeFrameDrainCallbacks {
    std::function<bool()> should_abort_before_receive;
    std::function<int(AVFrame*)> receive_frame;
    std::function<void(AVFrame*)> rescale_timestamps;
    std::function<void(const AVFrame*)> on_frame_ready;
    std::function<bool(AVFrame*)> publish_frame;
    std::function<bool()> should_stop_after_publish;
};

struct DecodeFrameDrainResult {
    bool clear_drain_request = false;
    bool stop_with_error = false;
    int frames_published = 0;
};

DecodeFrameDrainResult drain_frames_before_next_packet(
    AVFrame* frame,
    const DecodeFrameDrainCallbacks& callbacks);

} // namespace vr
