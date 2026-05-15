#include "video_renderer/decode/av_frame_lifetime.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace vr {

AvFrameUnrefGuard::AvFrameUnrefGuard(AVFrame* frame) noexcept
    : frame_(frame) {}

AvFrameUnrefGuard::~AvFrameUnrefGuard() {
    reset_now();
}

void AvFrameUnrefGuard::dismiss() noexcept {
    frame_ = nullptr;
}

void AvFrameUnrefGuard::reset_now() noexcept {
    reset_reusable_av_frame(frame_);
    frame_ = nullptr;
}

AVFrame* AvFrameUnrefGuard::get() const noexcept {
    return frame_;
}

void reset_reusable_av_frame(AVFrame* frame) noexcept {
    if (frame) {
        av_frame_unref(frame);
    }
}

} // namespace vr
