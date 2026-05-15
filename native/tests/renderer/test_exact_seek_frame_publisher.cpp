#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/decoded_frame_publisher.h"
#include "video_renderer/decode/exact_seek_candidate_store.h"
#include "video_renderer/decode/exact_seek_frame_publisher.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

using namespace vr;

namespace {

AVFrame* make_yuv420_frame(int64_t pts_us, int width = 16, int height = 16) {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    frame->pts = pts_us;
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

AVFrame* make_rgb_frame(int64_t pts_us) {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = 4;
    frame->height = 4;
    frame->pts = pts_us;
    REQUIRE(av_frame_get_buffer(frame, 0) >= 0);
    return frame;
}

void collect_candidate(ExactSeekCandidateStore& store, AVFrame* frame) {
    auto candidate = ExactSeekCandidateStore::make_candidate(frame);
    REQUIRE(candidate.frame != nullptr);
    store.collect(std::move(candidate), 0, nullptr);
}

} // namespace

TEST_CASE("ExactSeekFramePublisher: publishes selected preview window and clears reorder",
          "[decode_thread][exact_seek_frame_publisher]") {
    ExactSeekCandidateStore store;
    for (int64_t pts : {100, 133, 166, 200, 233}) {
        AVFrame* frame = make_yuv420_frame(pts);
        collect_candidate(store, frame);
        av_frame_free(&frame);
    }

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

    auto result = publish_exact_seek_preview_frames(
        store,
        1,
        output_buffer,
        publisher,
        hw_enabled,
        nullptr,
        hw_visibility_flush_pending);

    REQUIRE(result.can_publish);
    REQUIRE_FALSE(result.conversion_failed);
    REQUIRE(result.selected_pts_us == 133);
    REQUIRE(result.published_count == 4);
    REQUIRE(result.pending_count == 0);
    REQUIRE(output_buffer.total_count() == 4);
    REQUIRE(store.reorder_empty());
    REQUIRE(store.pending_empty());
}

TEST_CASE("ExactSeekFramePublisher: publishes pending exact-seek candidate",
          "[decode_thread][exact_seek_frame_publisher]") {
    ExactSeekCandidateStore store;
    for (int64_t pts : {100, 200, 300}) {
        AVFrame* frame = make_yuv420_frame(pts);
        collect_candidate(store, frame);
        av_frame_free(&frame);
    }
    store.move_reorder_tail_to_pending(1);

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

    REQUIRE(publish_pending_exact_seek_frame(store, publisher));

    REQUIRE(output_buffer.total_count() == 1);
    REQUIRE(store.pending_count() == 1);
    REQUIRE_FALSE(decode_paused.load(std::memory_order_acquire));
    REQUIRE(running.load(std::memory_order_acquire));
}

TEST_CASE("ExactSeekFramePublisher: conversion failure clears candidates",
          "[decode_thread][exact_seek_frame_publisher]") {
    ExactSeekCandidateStore store;
    AVFrame* frame = make_rgb_frame(7);
    collect_candidate(store, frame);
    av_frame_free(&frame);

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

    auto result = publish_exact_seek_preview_frames(
        store,
        0,
        output_buffer,
        publisher,
        hw_enabled,
        nullptr,
        hw_visibility_flush_pending);

    REQUIRE(result.can_publish);
    REQUIRE(result.conversion_failed);
    REQUIRE(output_buffer.state() == TrackState::Error);
    REQUIRE(decode_paused.load(std::memory_order_acquire));
    REQUIRE_FALSE(running.load(std::memory_order_acquire));
    REQUIRE(store.reorder_empty());
    REQUIRE(store.pending_empty());
}
