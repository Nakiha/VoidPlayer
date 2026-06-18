#include <catch2/catch_test_macros.hpp>

#include "renderer/decode/codec_loop.h"

#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

using namespace vr;

TEST_CASE("CodecLoop: codec return values are classified explicitly",
          "[decode_thread][codec_loop]") {
    REQUIRE(classify_codec_loop_result(0) == CodecLoopResult::Ok);
    REQUIRE(classify_codec_loop_result(1) == CodecLoopResult::Ok);

    REQUIRE(classify_codec_loop_result(AVERROR(EAGAIN)) == CodecLoopResult::Again);
    REQUIRE(codec_loop_is_again_or_eof(AVERROR(EAGAIN)));

    REQUIRE(classify_codec_loop_result(AVERROR_EOF) == CodecLoopResult::EndOfStream);
    REQUIRE(codec_loop_is_again_or_eof(AVERROR_EOF));

    REQUIRE(classify_codec_loop_result(codec_loop_seh_caught_code()) ==
            CodecLoopResult::SehCaught);
    REQUIRE(codec_loop_is_seh_caught(codec_loop_seh_caught_code()));

    REQUIRE(classify_codec_loop_result(AVERROR_INVALIDDATA) == CodecLoopResult::Error);
    REQUIRE_FALSE(codec_loop_is_again_or_eof(AVERROR_INVALIDDATA));
}

TEST_CASE("CodecLoop: send receive wrappers fail closed for unopened codec contexts",
          "[decode_thread][codec_loop]") {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    REQUIRE(codec != nullptr);

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    REQUIRE(ctx != nullptr);
    AVPacket* packet = av_packet_alloc();
    REQUIRE(packet != nullptr);
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    std::recursive_mutex device_mutex;

    const int send_ret =
        send_codec_packet_seh_guarded(ctx, packet, true, &device_mutex);
    REQUIRE(send_ret < 0);
    REQUIRE(classify_codec_loop_result(send_ret) == CodecLoopResult::Error);

    const int receive_ret =
        receive_codec_frame_seh_guarded(ctx, frame, true, &device_mutex);
    REQUIRE(receive_ret < 0);
    REQUIRE(classify_codec_loop_result(receive_ret) == CodecLoopResult::Error);

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
}

TEST_CASE("CodecLoop: open wrapper supports injected open function",
          "[decode_thread][codec_loop]") {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    REQUIRE(codec != nullptr);

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    REQUIRE(ctx != nullptr);

    bool called = false;
    auto open_fn = [](AVCodecContext*, const AVCodec*, AVDictionary**) -> int {
        return AVERROR_INVALIDDATA;
    };
    const int ret = open_codec_seh_guarded(ctx, codec, nullptr, open_fn);
    called = true;

    REQUIRE(called);
    REQUIRE(ret == AVERROR_INVALIDDATA);
    REQUIRE(classify_codec_loop_result(ret) == CodecLoopResult::Error);

    avcodec_free_context(&ctx);
}
