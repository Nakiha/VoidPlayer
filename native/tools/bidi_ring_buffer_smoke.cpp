#include "renderer/buffer/bidi_ring_buffer.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

vr::TextureFrame make_frame_with_retire_delay(int64_t pts_us, int sleep_ms) {
    vr::TextureFrame frame;
    frame.pts_us = pts_us;
    auto* data = new std::vector<uint8_t>(6 * 1024 * 1024);
    frame.storage = vr::CpuRgbaFrameStorage{
        std::shared_ptr<std::vector<uint8_t>>(data, [sleep_ms](std::vector<uint8_t>* p) {
            if (sleep_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            }
            delete p;
        }),
        0,
    };
    return frame;
}

} // namespace

int main() {
    vr::BidiRingBuffer ring(/*forward_depth=*/1, /*backward_depth=*/1);

    vr::BidiRingBuffer::PushTiming timing;
    if (!ring.push(make_frame_with_retire_delay(0, 20), &timing) ||
        !ring.push(make_frame_with_retire_delay(1, 0), &timing)) {
        return fail("failed to prime bidi ring");
    }
    if (!ring.advance() || !ring.advance()) {
        return fail("failed to consume primed frames");
    }
    if (!ring.push(make_frame_with_retire_delay(2, 0), &timing)) {
        return fail("failed to push replacement frame");
    }

    const auto start = std::chrono::steady_clock::now();
    if (!ring.push(make_frame_with_retire_delay(3, 0), &timing)) {
        return fail("failed to push overwrite frame");
    }
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    if (timing.overwritten_cpu_bytes < 6u * 1024u * 1024u) {
        return fail("overwrite did not retire expected large frame");
    }
    if (timing.assign_us >= 5000) {
        return fail("large frame retirement happened inside ring assign timing");
    }
    if (elapsed_us < 15000) {
        return fail("test did not observe delayed retired frame destruction");
    }
    return 0;
}
