#include <catch2/catch_test_macros.hpp>
#include "video_renderer/decode/frame_converter.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

using namespace vr;

TEST_CASE("FrameConverter: init_software YUV420P succeeds", "[frame_converter]") {
    FrameConverter converter;
    bool ok = converter.init_software(1920, 1080, AV_PIX_FMT_YUV420P);
    REQUIRE(ok == true);
    REQUIRE(converter.is_hardware() == false);
}

TEST_CASE("FrameConverter: init_software NV12 succeeds", "[frame_converter]") {
    FrameConverter converter;
    bool ok = converter.init_software(1920, 1080, AV_PIX_FMT_NV12);
    REQUIRE(ok == true);
    REQUIRE(converter.is_hardware() == false);
}

TEST_CASE("FrameConverter: convert white YUV420P frame", "[frame_converter]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(64, 64, AV_PIX_FMT_YUV420P));

    // Create a dummy AVFrame
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 64;
    frame->height = 64;

    int ret = av_frame_get_buffer(frame, 0);
    REQUIRE(ret >= 0);

    // Fill Y plane with 255, U and V with 128 (white in YUV)
    for (int i = 0; i < frame->height; ++i) {
        memset(frame->data[0] + i * frame->linesize[0], 255, frame->width);
    }
    for (int i = 0; i < frame->height / 2; ++i) {
        memset(frame->data[1] + i * frame->linesize[1], 128, frame->width / 2);
        memset(frame->data[2] + i * frame->linesize[2], 128, frame->width / 2);
    }

    // Set PTS explicitly (av_frame_alloc defaults to AV_NOPTS_VALUE)
    frame->pts = 0;
    TextureFrame result = converter.convert(frame);
    REQUIRE(result.pts_us == 0);
    REQUIRE(result.texture_handle != nullptr);
    REQUIRE(result.is_ref == false);

    // Verify RGBA output is white (all 0xFF) for at least the first few pixels
    uint8_t* rgba = static_cast<uint8_t*>(result.texture_handle);
    // White in RGBA = 0xFF,0xFF,0xFF,0xFF
    REQUIRE(rgba[0] == 255); // R
    REQUIRE(rgba[1] == 255); // G
    REQUIRE(rgba[2] == 255); // B
    REQUIRE(rgba[3] == 255); // A

    // Clean up (cpu_data shared_ptr handles RGBA buffer lifetime)
    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: convert preserves PTS", "[frame_converter]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(64, 64, AV_PIX_FMT_YUV420P));

    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 64;
    frame->height = 64;
    frame->pts = 123456;

    int ret = av_frame_get_buffer(frame, 0);
    REQUIRE(ret >= 0);

    // Fill with zeros (black)
    for (int p = 0; p < 3; ++p) {
        int h = (p == 0) ? frame->height : frame->height / 2;
        for (int i = 0; i < h; ++i) {
            memset(frame->data[p] + i * frame->linesize[p], 0, frame->linesize[p]);
        }
    }

    TextureFrame result = converter.convert(frame);
    REQUIRE(result.pts_us == 123456);
    REQUIRE(result.texture_handle != nullptr);

    // cpu_data shared_ptr handles RGBA buffer lifetime
    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: init_hardware sets hardware mode", "[frame_converter]") {
    FrameConverter converter;
    // Pass null pointers since we are not creating a real D3D11 device in tests
    bool ok = converter.init_hardware(nullptr, nullptr, 1920, 1080);
    REQUIRE(ok == true);
    REQUIRE(converter.is_hardware() == true);
}
