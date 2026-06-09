#include "media/ffmpeg_lifetime.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
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

AVCodecContext* AvCodecContextOwner::operator->() const noexcept {
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

AvFormatContextOwner::AvFormatContextOwner(AVFormatContext* context) noexcept
    : context_(context) {}

AvFormatContextOwner::~AvFormatContextOwner() {
    reset();
}

AvFormatContextOwner::AvFormatContextOwner(AvFormatContextOwner&& other) noexcept
    : context_(other.release()) {}

AvFormatContextOwner& AvFormatContextOwner::operator=(AvFormatContextOwner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

AvFormatContextOwner AvFormatContextOwner::allocate() noexcept {
    return AvFormatContextOwner(avformat_alloc_context());
}

AVFormatContext* AvFormatContextOwner::get() const noexcept {
    return context_;
}

AVFormatContext* AvFormatContextOwner::operator->() const noexcept {
    return context_;
}

AVFormatContext* AvFormatContextOwner::release() noexcept {
    AVFormatContext* context = context_;
    context_ = nullptr;
    return context;
}

AVFormatContext** AvFormatContextOwner::mutable_address() noexcept {
    return &context_;
}

void AvFormatContextOwner::reset(AVFormatContext* context) noexcept {
    if (context_) {
        avformat_close_input(&context_);
    }
    context_ = context;
}

AvFormatContextOwner::operator bool() const noexcept {
    return context_ != nullptr;
}

AvBufferRefOwner::AvBufferRefOwner(AVBufferRef* ref) noexcept
    : ref_(ref) {}

AvBufferRefOwner::~AvBufferRefOwner() {
    reset();
}

AvBufferRefOwner::AvBufferRefOwner(AvBufferRefOwner&& other) noexcept
    : ref_(other.release()) {}

AvBufferRefOwner& AvBufferRefOwner::operator=(AvBufferRefOwner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

AvBufferRefOwner AvBufferRefOwner::allocate(size_t size) noexcept {
    return AvBufferRefOwner(av_buffer_alloc(size));
}

AVBufferRef* AvBufferRefOwner::get() const noexcept {
    return ref_;
}

AVBufferRef* AvBufferRefOwner::operator->() const noexcept {
    return ref_;
}

AVBufferRef* AvBufferRefOwner::release() noexcept {
    AVBufferRef* ref = ref_;
    ref_ = nullptr;
    return ref;
}

AVBufferRef** AvBufferRefOwner::put() noexcept {
    reset();
    return &ref_;
}

void AvBufferRefOwner::reset(AVBufferRef* ref) noexcept {
    if (ref_) {
        av_buffer_unref(&ref_);
    }
    ref_ = ref;
}

AvBufferRefOwner::operator bool() const noexcept {
    return ref_ != nullptr;
}

} // namespace vr
