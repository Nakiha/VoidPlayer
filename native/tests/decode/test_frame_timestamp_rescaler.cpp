#include <catch2/catch_test_macros.hpp>

#include "renderer/decode/frame_timestamp_rescaler.h"

extern "C" {
#include <libavutil/frame.h>
}

using namespace vr;

TEST_CASE("FrameTimestampRescaler: prefers best effort and rescales timestamps",
          "[decode_thread][frame_timestamp_rescaler]") {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->pts = 90;
    frame->best_effort_timestamp = 180;
    frame->pkt_dts = 45;
    frame->duration = 90;

    rescale_frame_timestamps_to_us(frame, AVRational{1, 90000});

    REQUIRE(frame->pts == 2000);
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

TEST_CASE("FrameTimestampNormalizer: preserves decoder order and every frame",
          "[decode_thread][frame_timestamp_rescaler][hevc_timestamp]") {
    FrameTimestampNormalizer normalizer(AVRational{1, 1200000});
    const int64_t raw_pts[] = {0, 19999, 59997, 39998, 79997};
    const int64_t best_effort_pts[] = {0, 19999, 59997, 99996, 79997};
    const int64_t expected_pts_us[] = {0, 16666, 49998, 83330, 99996};
    const int markers[] = {10, 11, 12, 13, 14};
    int64_t previous_pts_us = INT64_MIN;

    for (size_t i = 0; i < std::size(raw_pts); ++i) {
        AVFrame* frame = av_frame_alloc();
        REQUIRE(frame != nullptr);
        frame->pts = raw_pts[i];
        frame->best_effort_timestamp = best_effort_pts[i];
        frame->pkt_dts = raw_pts[i];
        frame->duration = 19999;
        frame->opaque = reinterpret_cast<void*>(
            static_cast<uintptr_t>(markers[i]));

        const auto result = normalizer.normalize(frame);

        INFO("frame=" << i << " pts_us=" << frame->pts);
        REQUIRE(frame->pts > previous_pts_us);
        REQUIRE(frame->pts == expected_pts_us[i]);
        REQUIRE(reinterpret_cast<uintptr_t>(frame->opaque) ==
                static_cast<uintptr_t>(markers[i]));
        REQUIRE(result.output_pts_us == frame->pts);
        previous_pts_us = frame->pts;
        av_frame_free(&frame);
    }
}

TEST_CASE("FrameTimestampNormalizer: reset starts an independent seek epoch",
          "[decode_thread][frame_timestamp_rescaler]") {
    FrameTimestampNormalizer normalizer(AVRational{1, 1000});
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->pts = 5000;
    frame->best_effort_timestamp = 5000;
    frame->duration = 16;
    normalizer.normalize(frame);
    REQUIRE(frame->pts == 5000000);

    normalizer.reset();
    av_frame_unref(frame);
    frame->pts = 100;
    frame->best_effort_timestamp = 100;
    frame->duration = 16;
    const auto result = normalizer.normalize(frame);

    REQUIRE(frame->pts == 100000);
    REQUIRE_FALSE(result.adjusted_for_monotonicity);
    REQUIRE(result.adjustment_count == 0);
    av_frame_free(&frame);
}

TEST_CASE("FrameTimestampRescaler: synthesizes a timestamp for an undated frame",
          "[decode_thread][frame_timestamp_rescaler]") {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->pts = AV_NOPTS_VALUE;
    frame->best_effort_timestamp = AV_NOPTS_VALUE;
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->duration = -1;

    rescale_frame_timestamps_to_us(frame, AVRational{1, 90000});

    REQUIRE(frame->pts == 0);
    REQUIRE(frame->pkt_dts == AV_NOPTS_VALUE);
    REQUIRE(frame->duration == -1);

    av_frame_free(&frame);
}
