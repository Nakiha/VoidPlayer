#include "video_renderer/decode/decoded_frame_sink.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <vector>

int main() {
    vr::TrackBuffer buffer(2, 1);
    std::atomic<bool> decode_paused{false};
    std::atomic<bool> running{true};
    vr::TrackBufferDecodedFrameSink sink(buffer, decode_paused, running);

    vr::TextureFrame frame;
    frame.pts_us = 1234;
    frame.duration_us = 40000;
    frame.width = 2;
    frame.height = 2;
    frame.cpu_data = std::make_shared<std::vector<uint8_t>>(16, 0x7f);
    frame.storage = vr::CpuRgbaFrameStorage{
        frame.cpu_data,
        8,
    };

    sink.publish_decoded_frame(std::move(frame));
    const auto queued = buffer.peek();
    if (!queued.has_value()) {
        std::cerr << "decoded frame sink did not publish to TrackBuffer\n";
        return 1;
    }
    if (queued->pts_us != 1234 || queued->width != 2 || queued->height != 2) {
        std::cerr << "decoded frame sink corrupted frame metadata\n";
        return 1;
    }

    sink.fail_decoded_frame_publish("smoke");
    if (buffer.state() != vr::TrackState::Error) {
        std::cerr << "decoded frame sink did not mark TrackBuffer error\n";
        return 1;
    }
    if (!decode_paused.load(std::memory_order_acquire) ||
        running.load(std::memory_order_acquire)) {
        std::cerr << "decoded frame sink did not stop decode state\n";
        return 1;
    }

    return 0;
}
