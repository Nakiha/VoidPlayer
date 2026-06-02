#include <catch2/catch_test_macros.hpp>

#include "renderer/decode/codec_loop.h"
#include "renderer/decode/decode_frame_drainer.h"

extern "C" {
#include <libavutil/error.h>
}

using namespace vr;

TEST_CASE("DecodeFrameDrainer: publishes frames until receive stops",
          "[decode_thread][decode_frame_drainer]") {
    int receive_calls = 0;
    int rescale_calls = 0;
    int ready_calls = 0;
    int publish_calls = 0;

    const auto result = drain_frames_before_next_packet(
        nullptr,
        DecodeFrameDrainCallbacks{
            []() { return false; },
            [&](AVFrame*) {
                ++receive_calls;
                return receive_calls <= 2 ? 0 : AVERROR(EAGAIN);
            },
            [&](AVFrame*) { ++rescale_calls; },
            [&](const AVFrame*) { ++ready_calls; },
            [&](AVFrame*) {
                ++publish_calls;
                return true;
            },
            []() { return false; },
        });

    REQUIRE(result.frames_published == 2);
    REQUIRE(result.clear_drain_request);
    REQUIRE_FALSE(result.stop_with_error);
    REQUIRE(receive_calls == 3);
    REQUIRE(rescale_calls == 2);
    REQUIRE(ready_calls == 2);
    REQUIRE(publish_calls == 2);
}

TEST_CASE("DecodeFrameDrainer: clears drain request on abort before receive",
          "[decode_thread][decode_frame_drainer]") {
    int receive_calls = 0;
    const auto result = drain_frames_before_next_packet(
        nullptr,
        DecodeFrameDrainCallbacks{
            []() { return true; },
            [&](AVFrame*) {
                ++receive_calls;
                return 0;
            },
            {},
            {},
            {},
            {},
        });

    REQUIRE(result.frames_published == 0);
    REQUIRE(result.clear_drain_request);
    REQUIRE_FALSE(result.stop_with_error);
    REQUIRE(receive_calls == 0);
}

TEST_CASE("DecodeFrameDrainer: reports codec fatal errors",
          "[decode_thread][decode_frame_drainer]") {
    const auto result = drain_frames_before_next_packet(
        nullptr,
        DecodeFrameDrainCallbacks{
            []() { return false; },
            [](AVFrame*) { return codec_loop_seh_caught_code(); },
            {},
            {},
            {},
            {},
        });

    REQUIRE(result.frames_published == 0);
    REQUIRE(result.clear_drain_request);
    REQUIRE(result.stop_with_error);
}

TEST_CASE("DecodeFrameDrainer: preserves drain request when publish fails",
          "[decode_thread][decode_frame_drainer]") {
    const auto result = drain_frames_before_next_packet(
        nullptr,
        DecodeFrameDrainCallbacks{
            []() { return false; },
            [](AVFrame*) { return 0; },
            {},
            {},
            [](AVFrame*) { return false; },
            []() { return false; },
        });

    REQUIRE(result.frames_published == 0);
    REQUIRE_FALSE(result.clear_drain_request);
    REQUIRE_FALSE(result.stop_with_error);
}
