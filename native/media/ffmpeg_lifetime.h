#pragma once

#include <cstddef>

struct AVCodec;
struct AVCodecContext;
struct AVBufferRef;
struct AVFormatContext;
struct SwrContext;

namespace vr {

class AvCodecContextOwner {
public:
    AvCodecContextOwner() noexcept = default;
    explicit AvCodecContextOwner(AVCodecContext* context) noexcept;
    ~AvCodecContextOwner();

    AvCodecContextOwner(const AvCodecContextOwner&) = delete;
    AvCodecContextOwner& operator=(const AvCodecContextOwner&) = delete;

    AvCodecContextOwner(AvCodecContextOwner&& other) noexcept;
    AvCodecContextOwner& operator=(AvCodecContextOwner&& other) noexcept;

    static AvCodecContextOwner allocate(const AVCodec* codec) noexcept;
    AVCodecContext* get() const noexcept;
    AVCodecContext* operator->() const noexcept;
    AVCodecContext* release() noexcept;
    void reset(AVCodecContext* context = nullptr) noexcept;
    explicit operator bool() const noexcept;

private:
    AVCodecContext* context_ = nullptr;
};

class SwrContextOwner {
public:
    SwrContextOwner() noexcept = default;
    explicit SwrContextOwner(SwrContext* context) noexcept;
    ~SwrContextOwner();

    SwrContextOwner(const SwrContextOwner&) = delete;
    SwrContextOwner& operator=(const SwrContextOwner&) = delete;

    SwrContextOwner(SwrContextOwner&& other) noexcept;
    SwrContextOwner& operator=(SwrContextOwner&& other) noexcept;

    SwrContext* get() const noexcept;
    SwrContext* release() noexcept;
    SwrContext** put() noexcept;
    void reset(SwrContext* context = nullptr) noexcept;
    explicit operator bool() const noexcept;

private:
    SwrContext* context_ = nullptr;
};

class AvFormatContextOwner {
public:
    AvFormatContextOwner() noexcept = default;
    explicit AvFormatContextOwner(AVFormatContext* context) noexcept;
    ~AvFormatContextOwner();

    AvFormatContextOwner(const AvFormatContextOwner&) = delete;
    AvFormatContextOwner& operator=(const AvFormatContextOwner&) = delete;

    AvFormatContextOwner(AvFormatContextOwner&& other) noexcept;
    AvFormatContextOwner& operator=(AvFormatContextOwner&& other) noexcept;

    static AvFormatContextOwner allocate() noexcept;
    AVFormatContext* get() const noexcept;
    AVFormatContext* operator->() const noexcept;
    AVFormatContext* release() noexcept;
    AVFormatContext** mutable_address() noexcept;
    void reset(AVFormatContext* context = nullptr) noexcept;
    explicit operator bool() const noexcept;

private:
    AVFormatContext* context_ = nullptr;
};

class AvBufferRefOwner {
public:
    AvBufferRefOwner() noexcept = default;
    explicit AvBufferRefOwner(AVBufferRef* ref) noexcept;
    ~AvBufferRefOwner();

    AvBufferRefOwner(const AvBufferRefOwner&) = delete;
    AvBufferRefOwner& operator=(const AvBufferRefOwner&) = delete;

    AvBufferRefOwner(AvBufferRefOwner&& other) noexcept;
    AvBufferRefOwner& operator=(AvBufferRefOwner&& other) noexcept;

    static AvBufferRefOwner allocate(size_t size) noexcept;
    AVBufferRef* get() const noexcept;
    AVBufferRef* operator->() const noexcept;
    AVBufferRef* release() noexcept;
    AVBufferRef** put() noexcept;
    void reset(AVBufferRef* ref = nullptr) noexcept;
    explicit operator bool() const noexcept;

private:
    AVBufferRef* ref_ = nullptr;
};

} // namespace vr
