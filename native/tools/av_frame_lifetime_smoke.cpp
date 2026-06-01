#include "video_renderer/decode/av_frame_lifetime.h"

#include <iostream>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
}

namespace {

int fail(const char* message) {
    std::cerr << message << "\n";
    return 1;
}

} // namespace

int main() {
    auto owner = vr::AvFrameOwner::allocate();
    if (!owner || !owner.get()) {
        return fail("AvFrameOwner did not allocate a frame");
    }

    AVFrame* raw = owner.release();
    if (!raw || owner) {
        av_frame_free(&raw);
        return fail("AvFrameOwner did not release ownership");
    }

    vr::AvFrameOwner moved(raw);
    raw = nullptr;
    vr::AvFrameOwner assigned;
    assigned = std::move(moved);
    if (moved || !assigned) {
        return fail("AvFrameOwner move ownership failed");
    }

    assigned.reset();
    if (assigned) {
        return fail("AvFrameOwner reset did not clear ownership");
    }

    return 0;
}
