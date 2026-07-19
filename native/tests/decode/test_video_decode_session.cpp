#include <catch2/catch_test_macros.hpp>

#include "media/video_decode_session.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

using namespace vr;

TEST_CASE("VideoDecodeSession: null parameters fail closed",
          "[video_decode_session][decode_thread]") {
    VideoDecodeSession session;
    std::string error;

    REQUIRE_FALSE(session.initialize(nullptr, {}, error));
    REQUIRE_FALSE(error.empty());
    REQUIRE_FALSE(session.is_valid());
}

TEST_CASE("VideoDecodeSession: owns copied parameters and codec lifetime",
          "[video_decode_session][decode_thread]") {
    AVCodecParameters* parameters = avcodec_parameters_alloc();
    REQUIRE(parameters != nullptr);
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = AV_CODEC_ID_H264;
    parameters->format = AV_PIX_FMT_YUV420P;
    parameters->width = 64;
    parameters->height = 64;

    VideoDecodeSessionOptions options;
    options.thread_count = 2;
    VideoDecodeSession session;
    std::string error;
    REQUIRE(session.initialize(parameters, options, error));

    avcodec_parameters_free(&parameters);
    REQUIRE(session.is_valid());
    REQUIRE(session.codec_parameters() != nullptr);
    REQUIRE(session.codec_parameters()->codec_id == AV_CODEC_ID_H264);
    REQUIRE(session.codec_context() != nullptr);
    REQUIRE(session.codec_context()->thread_count == 2);
    REQUIRE(session.open(error));
    REQUIRE(session.is_open());

    session.close();
    REQUIRE_FALSE(session.is_valid());
    REQUIRE_FALSE(session.is_open());
}

TEST_CASE("VideoDecodeSession: packet loop requires an open session",
          "[video_decode_session][decode_thread]") {
    VideoDecodeSession session;
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);

    REQUIRE(session.send_packet(nullptr) == AVERROR(EINVAL));
    REQUIRE(session.receive_frame(frame) == AVERROR(EINVAL));

    av_frame_free(&frame);
}
