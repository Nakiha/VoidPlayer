#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/av_frame_lifetime.h"

#include <utility>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}

using namespace vr;

namespace {

AVFrame* make_buffered_test_frame() {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->buf[0] = av_buffer_alloc(16);
    REQUIRE(frame->buf[0] != nullptr);
    frame->data[0] = frame->buf[0]->data;
    frame->linesize[0] = 16;
    frame->pts = 42;
    return frame;
}

} // namespace

TEST_CASE("AvFrameUnrefGuard: unrefs reusable frame on scope exit",
          "[decode_thread][av_frame_lifetime]") {
    AVFrame* frame = make_buffered_test_frame();
    {
        AvFrameUnrefGuard guard(frame);
        REQUIRE(guard.get() == frame);
        REQUIRE(frame->buf[0] != nullptr);
    }

    REQUIRE(frame->buf[0] == nullptr);
    REQUIRE(frame->data[0] == nullptr);

    av_frame_free(&frame);
}

TEST_CASE("AvFrameUnrefGuard: dismiss leaves ownership with caller",
          "[decode_thread][av_frame_lifetime]") {
    AVFrame* frame = make_buffered_test_frame();
    {
        AvFrameUnrefGuard guard(frame);
        guard.dismiss();
    }

    REQUIRE(frame->buf[0] != nullptr);

    reset_reusable_av_frame(frame);
    REQUIRE(frame->buf[0] == nullptr);

    av_frame_free(&frame);
}

TEST_CASE("AvFrameUnrefGuard: null frame reset is safe",
          "[decode_thread][av_frame_lifetime]") {
    reset_reusable_av_frame(nullptr);
    AvFrameUnrefGuard guard(nullptr);
    REQUIRE(guard.get() == nullptr);
}

TEST_CASE("AvFrameOwner: owns allocated frames",
          "[decode_thread][av_frame_lifetime]") {
    auto owner = AvFrameOwner::allocate();
    REQUIRE(owner);
    REQUIRE(owner.get() != nullptr);

    AVFrame* raw = owner.release();
    REQUIRE(raw != nullptr);
    REQUIRE_FALSE(owner);
    av_frame_free(&raw);
    REQUIRE(raw == nullptr);
}

TEST_CASE("AvFrameOwner: supports move-only ownership",
          "[decode_thread][av_frame_lifetime]") {
    auto owner = AvFrameOwner::allocate();
    REQUIRE(owner);
    AVFrame* raw = owner.get();

    AvFrameOwner moved(std::move(owner));
    REQUIRE_FALSE(owner);
    REQUIRE(moved.get() == raw);

    AvFrameOwner assigned;
    assigned = std::move(moved);
    REQUIRE_FALSE(moved);
    REQUIRE(assigned.get() == raw);
}
