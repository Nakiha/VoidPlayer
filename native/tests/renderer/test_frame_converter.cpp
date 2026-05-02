#include <catch2/catch_test_macros.hpp>
#include "video_renderer/decode/frame_converter.h"
#include <cstring>
#include <mutex>

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

    const int ret = av_frame_get_buffer(frame, 0);
    REQUIRE(ret >= 0);

    for (int y = 0; y < frame->height; ++y) {
        memset(frame->data[0] + y * frame->linesize[0], 96, frame->width);
    }
    for (int y = 0; y < frame->height / 2; ++y) {
        memset(frame->data[1] + y * frame->linesize[1], 128, frame->width / 2);
        memset(frame->data[2] + y * frame->linesize[2], 128, frame->width / 2);
    }
    return frame;
}

} // namespace

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
    REQUIRE(result.storage_kind() == FrameStorageKind::CpuRgba);
    REQUIRE(result.cpu_rgba_storage() != nullptr);
    REQUIRE(result.cpu_rgba_storage()->data == result.cpu_data);
    REQUIRE(result.cpu_rgba_storage()->stride == 64 * 4);

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
    REQUIRE(result.storage_kind() == FrameStorageKind::CpuRgba);
    REQUIRE(result.cpu_rgba_storage() != nullptr);
    REQUIRE(result.cpu_rgba_storage()->data == result.cpu_data);

    // cpu_data shared_ptr handles RGBA buffer lifetime
    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: software conversion follows dynamic frame geometry",
          "[frame_converter]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(64, 64, AV_PIX_FMT_YUV420P));

    AVFrame* first = make_yuv420_frame(64, 64, 1000);
    TextureFrame first_result = converter.convert(first);
    REQUIRE(first_result.texture_handle != nullptr);
    REQUIRE(first_result.width == 64);
    REQUIRE(first_result.height == 64);
    REQUIRE(first_result.cpu_rgba_storage() != nullptr);
    REQUIRE(first_result.cpu_rgba_storage()->stride == 64 * 4);

    AVFrame* second = make_yuv420_frame(96, 72, 2000);
    TextureFrame second_result = converter.convert(second);
    REQUIRE(second_result.texture_handle != nullptr);
    REQUIRE(second_result.width == 96);
    REQUIRE(second_result.height == 72);
    REQUIRE(second_result.cpu_rgba_storage() != nullptr);
    REQUIRE(second_result.cpu_rgba_storage()->stride == 96 * 4);
    REQUIRE(second_result.cpu_data != first_result.cpu_data);

    av_frame_free(&first);
    av_frame_free(&second);
}

TEST_CASE("FrameConverter: propagates color metadata", "[frame_converter][color]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(1920, 1080, AV_PIX_FMT_YUV420P));

    AVFrame* frame = make_yuv420_frame(1920, 1080, 3000);
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;

    TextureFrame result = converter.convert(frame);
    REQUIRE(result.texture_handle != nullptr);
    REQUIRE(result.color.range == VIDEO_COLOR_RANGE_LIMITED);
    REQUIRE(result.color.matrix == VIDEO_COLOR_MATRIX_BT709);
    REQUIRE(result.color.transfer == VIDEO_COLOR_TRANSFER_SDR);
    REQUIRE(result.color.primaries == VIDEO_COLOR_PRIMARIES_BT709);

    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: maps HDR transfer metadata", "[frame_converter][color]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(3840, 2160, AV_PIX_FMT_YUV420P10LE));

    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_YUV420P10LE;
    frame->width = 64;
    frame->height = 64;
    frame->pts = 4000;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT2020_NCL;
    frame->color_trc = AVCOL_TRC_SMPTE2084;
    frame->color_primaries = AVCOL_PRI_BT2020;
    REQUIRE(av_frame_get_buffer(frame, 0) >= 0);
    for (int p = 0; p < 3; ++p) {
        const int h = p == 0 ? frame->height : frame->height / 2;
        for (int y = 0; y < h; ++y) {
            memset(frame->data[p] + y * frame->linesize[p], 0, frame->linesize[p]);
        }
    }

    TextureFrame result = converter.convert(frame);
    REQUIRE(result.texture_handle != nullptr);
    REQUIRE(result.color.range == VIDEO_COLOR_RANGE_LIMITED);
    REQUIRE(result.color.matrix == VIDEO_COLOR_MATRIX_BT2020_NCL);
    REQUIRE(result.color.transfer == VIDEO_COLOR_TRANSFER_PQ);
    REQUIRE(result.color.primaries == VIDEO_COLOR_PRIMARIES_BT2020);

    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: init_hardware sets hardware mode", "[frame_converter]") {
    FrameConverter converter;
    std::recursive_mutex device_mutex;
    // Pass null pointers since we are not creating a real D3D11 device in tests
    bool ok = converter.init_hardware(
        nullptr,
        nullptr,
        1920,
        1080,
        HwDecodeType::D3D11VA,
        false,
        &device_mutex);
    REQUIRE(ok == true);
    REQUIRE(converter.is_hardware() == true);
}

TEST_CASE("TextureFrame: storage exposes D3D11 NV12 metadata", "[frame_storage]") {
    TextureFrame frame;
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(0x1234);
    auto ref = std::shared_ptr<void>(reinterpret_cast<void*>(0x5678), [](void*) {});

    frame.texture_handle = texture;
    frame.is_ref = true;
    frame.is_nv12 = true;
    frame.texture_array_index = 7;
    frame.hw_frame_ref = ref;
    frame.storage = D3D11Nv12FrameStorage{texture, 7, ref};

    REQUIRE(frame.storage_kind() == FrameStorageKind::D3D11Nv12);
    REQUIRE(frame.d3d11_nv12_storage() != nullptr);
    REQUIRE(frame.d3d11_nv12_storage()->texture == texture);
    REQUIRE(frame.d3d11_nv12_storage()->array_index == 7);
    REQUIRE(frame.d3d11_nv12_storage()->frame_ref == ref);
}
