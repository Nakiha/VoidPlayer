#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/frame_timestamp_rescaler.h"

extern "C" {
#include <libavutil/frame.h>
}

using namespace vr;

TEST_CASE("FrameTimestampRescaler: rescales PTS, DTS, and duration to microseconds",
          "[decode_thread][frame_timestamp_rescaler]") {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->pts = 90;
    frame->best_effort_timestamp = 180;
    frame->pkt_dts = 45;
    frame->duration = 90;

    rescale_frame_timestamps_to_us(frame, AVRational{1, 90000});

    REQUIRE(frame->pts == 1000);
    REQUIRE(frame->best_effort_timestamp == 180);
    REQUIRE(frame->pkt_dts == 500);
    REQUIRE(frame->duration == 1000);

    av_frame_free(&frame);
}

TEST_CASE("FrameTimestampRescaler: falls back to best effort PTS",
          "[decode_thread][frame_timestamp_rescaler]") {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->pts = AV_NOPTS_VALUE;
    frame->best_effort_timestamp = 180;
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->duration = 0;

    rescale_frame_timestamps_to_us(frame, AVRational{1, 90000});

    REQUIRE(frame->pts == 2000);
    REQUIRE(frame->pkt_dts == AV_NOPTS_VALUE);
    REQUIRE(frame->duration == 0);

    av_frame_free(&frame);
}

TEST_CASE("FrameTimestampRescaler: leaves missing timestamps untouched",
          "[decode_thread][frame_timestamp_rescaler]") {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->pts = AV_NOPTS_VALUE;
    frame->best_effort_timestamp = AV_NOPTS_VALUE;
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->duration = -1;

    rescale_frame_timestamps_to_us(frame, AVRational{1, 90000});

    REQUIRE(frame->pts == AV_NOPTS_VALUE);
    REQUIRE(frame->pkt_dts == AV_NOPTS_VALUE);
    REQUIRE(frame->duration == -1);

    av_frame_free(&frame);
}
