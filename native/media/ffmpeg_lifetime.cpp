#include "media/ffmpeg_lifetime.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

namespace vr {

AvCodecContextOwner::AvCodecContextOwner(AVCodecContext* context) noexcept
    : context_(context) {}

AvCodecContextOwner::~AvCodecContextOwner() {
    reset();
}

AvCodecContextOwner::AvCodecContextOwner(AvCodecContextOwner&& other) noexcept
    : context_(other.release()) {}

AvCodecContextOwner& AvCodecContextOwner::operator=(AvCodecContextOwner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

AvCodecContextOwner AvCodecContextOwner::allocate(const AVCodec* codec) noexcept {
    return AvCodecContextOwner(avcodec_alloc_context3(codec));
}

AVCodecContext* AvCodecContextOwner::get() const noexcept {
    return context_;
}

AVCodecContext* AvCodecContextOwner::release() noexcept {
    AVCodecContext* context = context_;
    context_ = nullptr;
    return context;
}

void AvCodecContextOwner::reset(AVCodecContext* context) noexcept {
    if (context_) {
        avcodec_free_context(&context_);
    }
    context_ = context;
}

AvCodecContextOwner::operator bool() const noexcept {
    return context_ != nullptr;
}

SwrContextOwner::SwrContextOwner(SwrContext* context) noexcept
    : context_(context) {}

SwrContextOwner::~SwrContextOwner() {
    reset();
}

SwrContextOwner::SwrContextOwner(SwrContextOwner&& other) noexcept
    : context_(other.release()) {}

SwrContextOwner& SwrContextOwner::operator=(SwrContextOwner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

SwrContext* SwrContextOwner::get() const noexcept {
    return context_;
}

SwrContext* SwrContextOwner::release() noexcept {
    SwrContext* context = context_;
    context_ = nullptr;
    return context;
}

SwrContext** SwrContextOwner::put() noexcept {
    reset();
    return &context_;
}

void SwrContextOwner::reset(SwrContext* context) noexcept {
    if (context_) {
        swr_free(&context_);
    }
    context_ = context;
}

SwrContextOwner::operator bool() const noexcept {
    return context_ != nullptr;
}

} // namespace vr
