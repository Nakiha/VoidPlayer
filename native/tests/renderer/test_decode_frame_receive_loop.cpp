#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/codec_loop.h"
#include "video_renderer/decode/decode_frame_receive_loop.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

using namespace vr;

TEST_CASE("DecodeFrameReceiveLoop: publishes normal frames until receive stops",
          "[decode_thread][decode_frame_receive_loop]") {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);

    int receive_calls = 0;
    int published = 0;
    int preroll_checks = 0;
    bool first_visible_marked = false;

    const auto result = receive_decode_frames_for_packet(
        frame,
        DecodeFrameReceiveLoopOptions{false, -1, false},
        DecodeFrameReceiveLoopCallbacks{
            []() { return false; },
            [&](AVFrame*) {
                ++receive_calls;
                return receive_calls <= 2 ? 0 : AVERROR(EAGAIN);
            },
            {},
            {},
            {},
            {},
            {},
            {},
            [&]() { first_visible_marked = true; },
            [&](AVFrame*) {
                ++published;
                return true;
            },
            [&]() { ++preroll_checks; },
            {},
        });

    REQUIRE(result.frames_produced == 2);
    REQUIRE_FALSE(result.stop_with_error);
    REQUIRE(receive_calls == 3);
    REQUIRE(published == 2);
    REQUIRE(preroll_checks == 2);
    REQUIRE(first_visible_marked);
    av_frame_free(&frame);
}

TEST_CASE("DecodeFrameReceiveLoop: collects exact seek frames after target window",
          "[decode_thread][decode_frame_receive_loop]") {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);

    int receive_calls = 0;
    int dropped = 0;
    int collected = 0;
    int preview_checks = 0;
    int previews_published = 0;

    const auto result = receive_decode_frames_for_packet(
        frame,
        DecodeFrameReceiveLoopOptions{true, 1'000'000, false},
        DecodeFrameReceiveLoopCallbacks{
            []() { return false; },
            [&](AVFrame* received_frame) {
                ++receive_calls;
                if (receive_calls == 1) {
                    received_frame->pts = 700'000;
                    return 0;
                }
                if (receive_calls == 2) {
                    received_frame->pts = 800'000;
                    return 0;
                }
                return AVERROR(EAGAIN);
            },
            {},
            {},
            [&]() { ++dropped; },
            [&](AVFrame*) { ++collected; },
            [&]() {
                ++preview_checks;
                return true;
            },
            [&]() { ++previews_published; },
            {},
            {},
            {},
            {},
        });

    REQUIRE(result.frames_produced == 1);
    REQUIRE_FALSE(result.stop_with_error);
    REQUIRE(dropped == 1);
    REQUIRE(collected == 1);
    REQUIRE(preview_checks == 1);
    REQUIRE(previews_published == 1);
    av_frame_free(&frame);
}

TEST_CASE("DecodeFrameReceiveLoop: reports fatal receive errors",
          "[decode_thread][decode_frame_receive_loop]") {
    const auto result = receive_decode_frames_for_packet(
        nullptr,
        DecodeFrameReceiveLoopOptions{},
        DecodeFrameReceiveLoopCallbacks{
            []() { return false; },
            [](AVFrame*) { return codec_loop_seh_caught_code(); },
            {},
            {},
            {},
            {},
            {},
            {},
            {},
            {},
            {},
            {},
        });

    REQUIRE(result.frames_produced == 0);
    REQUIRE(result.stop_with_error);
}
