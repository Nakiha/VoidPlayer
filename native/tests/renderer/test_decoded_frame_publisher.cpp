#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/decoded_frame_publisher.h"

#include <cstring>
#include <optional>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

using namespace vr;

namespace {

AVFrame* make_yuv420_frame(int width, int height, int64_t pts) {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    frame->pts = pts;
    REQUIRE(av_frame_get_buffer(frame, 0) >= 0);

    for (int y = 0; y < frame->height; ++y) {
        memset(frame->data[0] + y * frame->linesize[0], 96, frame->width);
    }
    const int chroma_width = (frame->width + 1) / 2;
    const int chroma_height = (frame->height + 1) / 2;
    for (int y = 0; y < chroma_height; ++y) {
        memset(frame->data[1] + y * frame->linesize[1], 128, chroma_width);
        memset(frame->data[2] + y * frame->linesize[2], 128, chroma_width);
    }
    return frame;
}

AVFrame* make_unsupported_rgb_frame() {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = 4;
    frame->height = 4;
    frame->pts = 7;
    REQUIRE(av_frame_get_buffer(frame, 0) >= 0);
    return frame;
}

} // namespace

TEST_CASE("DecodedFramePublisher: publishes converted software frame",
          "[decode_thread][decoded_frame_publisher]") {
    TrackBuffer output_buffer;
    FrameConverter converter;
    REQUIRE(converter.init_software(16, 16, AV_PIX_FMT_YUV420P));
    bool hw_enabled = false;
    bool hw_visibility_flush_pending = false;
    std::unique_ptr<HwDecodeProvider> hw_provider;
    std::atomic<bool> decode_paused{false};
    std::atomic<bool> running{true};

    DecodedFramePublisher publisher(output_buffer,
                                    converter,
                                    hw_enabled,
                                    hw_provider,
                                    hw_visibility_flush_pending,
                                    decode_paused,
                                    running);

    AVFrame* frame = make_yuv420_frame(16, 16, 123);
    REQUIRE(publisher.convert_and_push_frame(frame, "unit-test"));

    auto published = output_buffer.peek(0);
    REQUIRE(published.has_value());
    REQUIRE(published->pts_us == 123);
    REQUIRE(published->width == 16);
    REQUIRE(published->height == 16);
    REQUIRE_FALSE(decode_paused.load(std::memory_order_acquire));
    REQUIRE(running.load(std::memory_order_acquire));

    av_frame_free(&frame);
}

TEST_CASE("DecodedFramePublisher: conversion failure marks decode error",
          "[decode_thread][decoded_frame_publisher]") {
    TrackBuffer output_buffer;
    FrameConverter converter;
    REQUIRE(converter.init_software(0, 0, AV_PIX_FMT_NONE));
    bool hw_enabled = false;
    bool hw_visibility_flush_pending = false;
    std::unique_ptr<HwDecodeProvider> hw_provider;
    std::atomic<bool> decode_paused{false};
    std::atomic<bool> running{true};

    DecodedFramePublisher publisher(output_buffer,
                                    converter,
                                    hw_enabled,
                                    hw_provider,
                                    hw_visibility_flush_pending,
                                    decode_paused,
                                    running);

    AVFrame* frame = make_unsupported_rgb_frame();
    REQUIRE_FALSE(publisher.convert_and_push_frame(frame, "unit-test"));
    REQUIRE(output_buffer.state() == TrackState::Error);
    REQUIRE(decode_paused.load(std::memory_order_acquire));
    REQUIRE_FALSE(running.load(std::memory_order_acquire));
    REQUIRE_FALSE(output_buffer.peek(0).has_value());

    av_frame_free(&frame);
}

TEST_CASE("DecodedFramePublisher: publishes already converted frame",
          "[decode_thread][decoded_frame_publisher]") {
    TrackBuffer output_buffer;
    FrameConverter converter;
    bool hw_enabled = false;
    bool hw_visibility_flush_pending = false;
    std::unique_ptr<HwDecodeProvider> hw_provider;
    std::atomic<bool> decode_paused{false};
    std::atomic<bool> running{true};

    DecodedFramePublisher publisher(output_buffer,
                                    converter,
                                    hw_enabled,
                                    hw_provider,
                                    hw_visibility_flush_pending,
                                    decode_paused,
                                    running);

    TextureFrame frame;
    frame.pts_us = 55;
    frame.width = 8;
    frame.height = 6;

    REQUIRE(publisher.push_converted_frame(std::move(frame), "unit-test"));

    auto published = output_buffer.peek(0);
    REQUIRE(published.has_value());
    REQUIRE(published->pts_us == 55);
    REQUIRE(published->width == 8);
    REQUIRE(published->height == 6);
    REQUIRE_FALSE(decode_paused.load(std::memory_order_acquire));
    REQUIRE(running.load(std::memory_order_acquire));
}

TEST_CASE("DecodedFramePublisher: missing converted frame marks decode error",
          "[decode_thread][decoded_frame_publisher]") {
    TrackBuffer output_buffer;
    FrameConverter converter;
    bool hw_enabled = false;
    bool hw_visibility_flush_pending = false;
    std::unique_ptr<HwDecodeProvider> hw_provider;
    std::atomic<bool> decode_paused{false};
    std::atomic<bool> running{true};

    DecodedFramePublisher publisher(output_buffer,
                                    converter,
                                    hw_enabled,
                                    hw_provider,
                                    hw_visibility_flush_pending,
                                    decode_paused,
                                    running);

    REQUIRE_FALSE(publisher.push_converted_frame(std::nullopt, "unit-test"));
    REQUIRE(output_buffer.state() == TrackState::Error);
    REQUIRE(decode_paused.load(std::memory_order_acquire));
    REQUIRE_FALSE(running.load(std::memory_order_acquire));
    REQUIRE_FALSE(output_buffer.peek(0).has_value());
}
