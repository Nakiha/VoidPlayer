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

void reset_reusable_av_frame(AVFrame* frame) noexcept;

} // namespace vr
