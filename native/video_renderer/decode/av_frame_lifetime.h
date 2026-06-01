#pragma once

struct AVFrame;

namespace vr {

class AvFrameUnrefGuard {
public:
    explicit AvFrameUnrefGuard(AVFrame* frame) noexcept;
    ~AvFrameUnrefGuard();

    AvFrameUnrefGuard(const AvFrameUnrefGuard&) = delete;
    AvFrameUnrefGuard& operator=(const AvFrameUnrefGuard&) = delete;

    void dismiss() noexcept;
    void reset_now() noexcept;
    AVFrame* get() const noexcept;

private:
    AVFrame* frame_ = nullptr;
};

class AvFrameOwner {
public:
    AvFrameOwner() noexcept = default;
    explicit AvFrameOwner(AVFrame* frame) noexcept;
    ~AvFrameOwner();

    AvFrameOwner(const AvFrameOwner&) = delete;
    AvFrameOwner& operator=(const AvFrameOwner&) = delete;

    AvFrameOwner(AvFrameOwner&& other) noexcept;
    AvFrameOwner& operator=(AvFrameOwner&& other) noexcept;

    static AvFrameOwner allocate() noexcept;
    AVFrame* get() const noexcept;
    AVFrame* release() noexcept;
    void reset(AVFrame* frame = nullptr) noexcept;
    explicit operator bool() const noexcept;

private:
    AVFrame* frame_ = nullptr;
};

void reset_reusable_av_frame(AVFrame* frame) noexcept;

} // namespace vr
