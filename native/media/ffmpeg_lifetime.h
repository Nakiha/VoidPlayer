#pragma once

struct AVCodec;
struct AVCodecContext;
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

} // namespace vr
