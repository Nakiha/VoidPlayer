#include "renderer/decode/av_frame_lifetime.h"

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

AvFrameOwner::AvFrameOwner(AVFrame* frame) noexcept
    : frame_(frame) {}

AvFrameOwner::~AvFrameOwner() {
    reset();
}

AvFrameOwner::AvFrameOwner(AvFrameOwner&& other) noexcept
    : frame_(other.release()) {}

AvFrameOwner& AvFrameOwner::operator=(AvFrameOwner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

AvFrameOwner AvFrameOwner::allocate() noexcept {
    return AvFrameOwner(av_frame_alloc());
}

AVFrame* AvFrameOwner::get() const noexcept {
    return frame_;
}

AVFrame* AvFrameOwner::release() noexcept {
    AVFrame* frame = frame_;
    frame_ = nullptr;
    return frame;
}

void AvFrameOwner::reset(AVFrame* frame) noexcept {
    if (frame_) {
        av_frame_free(&frame_);
    }
    frame_ = frame;
}

AvFrameOwner::operator bool() const noexcept {
    return frame_ != nullptr;
}

void reset_reusable_av_frame(AVFrame* frame) noexcept {
    if (frame) {
        av_frame_unref(frame);
    }
}

} // namespace vr
