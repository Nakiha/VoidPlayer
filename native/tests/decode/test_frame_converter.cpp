#include <catch2/catch_test_macros.hpp>
#include "renderer/decode/frame_converter.h"
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <atomic>
#include <d3d11.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
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
    const int chroma_width = (frame->width + 1) / 2;
    const int chroma_height = (frame->height + 1) / 2;
    for (int y = 0; y < chroma_height; ++y) {
        memset(frame->data[1] + y * frame->linesize[1], 128, chroma_width);
        memset(frame->data[2] + y * frame->linesize[2], 128, chroma_width);
    }
    return frame;
}

void free_counted_buffer(void* opaque, uint8_t* data) {
    auto* free_count = static_cast<std::atomic<int>*>(opaque);
    free_count->fetch_add(1, std::memory_order_relaxed);
    av_free(data);
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

TEST_CASE("FrameConverter: init_software common 4:2:2 and 4:4:4 formats succeeds",
          "[frame_converter]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(1920, 1080, AV_PIX_FMT_YUV422P));
    REQUIRE(converter.init_software(1920, 1080, AV_PIX_FMT_YUV444P));
    REQUIRE(converter.init_software(1920, 1080, AV_PIX_FMT_YUV422P10LE));
    REQUIRE(converter.init_software(1920, 1080, AV_PIX_FMT_YUV444P10LE));
}

TEST_CASE("FrameConverter: wraps YUV420P frame as planar YUV", "[frame_converter]") {
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
    auto converted = converter.convert(frame);
    REQUIRE(converted.has_value());
    TextureFrame result = std::move(*converted);
    REQUIRE(result.pts_us == 0);
    REQUIRE(result.texture_handle != nullptr);
    REQUIRE(result.is_ref == false);
    REQUIRE(result.is_nv12 == false);
    REQUIRE(result.storage_kind() == FrameStorageKind::CpuPlanarYuv);
    REQUIRE(result.cpu_planar_yuv_storage() != nullptr);
    REQUIRE(result.cpu_nv12_storage() == nullptr);

    const auto* planar = result.cpu_planar_yuv_storage();
    REQUIRE(planar->bytes_per_sample == 1);
    REQUIRE(planar->plane_widths[0] == 64);
    REQUIRE(planar->plane_heights[0] == 64);
    REQUIRE(planar->plane_widths[1] == 32);
    REQUIRE(planar->plane_heights[1] == 32);
    REQUIRE(planar->plane_widths[2] == 32);
    REQUIRE(planar->plane_heights[2] == 32);
    REQUIRE(planar->planes[0][0] == 255);
    REQUIRE(planar->planes[1][0] == 128);
    REQUIRE(planar->planes[2][0] == 128);

    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: wraps odd YUV420P frame with ceil chroma planes",
          "[frame_converter]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(65, 63, AV_PIX_FMT_YUV420P));

    AVFrame* frame = make_yuv420_frame(65, 63, 17);
    auto converted = converter.convert(frame);
    REQUIRE(converted.has_value());
    TextureFrame result = std::move(*converted);
    REQUIRE(result.width == 65);
    REQUIRE(result.height == 63);
    REQUIRE(result.storage_kind() == FrameStorageKind::CpuPlanarYuv);
    REQUIRE(result.cpu_planar_yuv_storage() != nullptr);

    const auto* planar = result.cpu_planar_yuv_storage();
    REQUIRE(planar->plane_widths[0] == 65);
    REQUIRE(planar->plane_heights[0] == 63);
    REQUIRE(planar->plane_widths[1] == 33);
    REQUIRE(planar->plane_heights[1] == 32);
    REQUIRE(planar->plane_widths[2] == 33);
    REQUIRE(planar->plane_heights[2] == 32);

    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: converts YUV422P to NV12 with vertical chroma downsample",
          "[frame_converter][color]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(4, 4, AV_PIX_FMT_YUV422P));

    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_YUV422P;
    frame->width = 4;
    frame->height = 4;
    REQUIRE(av_frame_get_buffer(frame, 0) >= 0);

    for (int y = 0; y < 4; ++y) {
        memset(frame->data[0] + y * frame->linesize[0], 64, 4);
    }
    const uint8_t u_rows[4][2] = {{10, 20}, {30, 40}, {50, 60}, {70, 80}};
    const uint8_t v_rows[4][2] = {{100, 110}, {120, 130}, {140, 150}, {160, 170}};
    for (int y = 0; y < 4; ++y) {
        memcpy(frame->data[1] + y * frame->linesize[1], u_rows[y], 2);
        memcpy(frame->data[2] + y * frame->linesize[2], v_rows[y], 2);
    }

    auto converted = converter.convert(frame);
    REQUIRE(converted.has_value());
    TextureFrame result = std::move(*converted);
    REQUIRE(result.is_nv12);
    REQUIRE_FALSE(result.is_p010);
    REQUIRE(result.cpu_nv12_storage() != nullptr);

    const auto* nv12 = static_cast<const uint8_t*>(result.texture_handle);
    const size_t uv_offset = static_cast<size_t>(result.cpu_nv12_storage()->y_stride) * 4;
    REQUIRE(nv12[uv_offset + 0] == 20);
    REQUIRE(nv12[uv_offset + 1] == 110);
    REQUIRE(nv12[uv_offset + 2] == 30);
    REQUIRE(nv12[uv_offset + 3] == 120);
    REQUIRE(nv12[uv_offset + 4] == 60);
    REQUIRE(nv12[uv_offset + 5] == 150);
    REQUIRE(nv12[uv_offset + 6] == 70);
    REQUIRE(nv12[uv_offset + 7] == 160);

    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: packs odd YUV444P to padded CPU NV12",
          "[frame_converter][color]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(5, 3, AV_PIX_FMT_YUV444P));

    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_YUV444P;
    frame->width = 5;
    frame->height = 3;
    REQUIRE(av_frame_get_buffer(frame, 0) >= 0);

    for (int y = 0; y < frame->height; ++y) {
        for (int x = 0; x < frame->width; ++x) {
            frame->data[0][y * frame->linesize[0] + x] = static_cast<uint8_t>(10 + y * 5 + x);
            frame->data[1][y * frame->linesize[1] + x] = 100;
            frame->data[2][y * frame->linesize[2] + x] = 150;
        }
    }

    auto converted = converter.convert(frame);
    REQUIRE(converted.has_value());
    TextureFrame result = std::move(*converted);
    REQUIRE(result.width == 5);
    REQUIRE(result.height == 3);
    REQUIRE(result.is_nv12);
    REQUIRE(result.cpu_nv12_storage() != nullptr);

    const auto* storage = result.cpu_nv12_storage();
    REQUIRE(storage->coded_width == 6);
    REQUIRE(storage->coded_height == 4);
    REQUIRE(storage->y_stride == 6);
    REQUIRE(storage->uv_stride == 6);

    const auto* nv12 = static_cast<const uint8_t*>(result.texture_handle);
    REQUIRE(nv12[5] == nv12[4]);
    REQUIRE(nv12[3 * storage->y_stride + 0] == nv12[2 * storage->y_stride + 0]);

    const size_t uv_offset = static_cast<size_t>(storage->y_stride) * storage->coded_height;
    for (int y = 0; y < storage->coded_height / 2; ++y) {
        for (int x = 0; x < storage->coded_width; x += 2) {
            REQUIRE(nv12[uv_offset + static_cast<size_t>(y) * storage->uv_stride + x] == 100);
            REQUIRE(nv12[uv_offset + static_cast<size_t>(y) * storage->uv_stride + x + 1] == 150);
        }
    }

    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: converts NV21 to NV12 channel order",
          "[frame_converter][color]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(4, 4, AV_PIX_FMT_NV21));

    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_NV21;
    frame->width = 4;
    frame->height = 4;
    REQUIRE(av_frame_get_buffer(frame, 0) >= 0);

    for (int y = 0; y < 4; ++y) {
        memset(frame->data[0] + y * frame->linesize[0], 64, 4);
    }
    for (int y = 0; y < 2; ++y) {
        uint8_t* row = frame->data[1] + y * frame->linesize[1];
        row[0] = 200;
        row[1] = 100;
        row[2] = 210;
        row[3] = 110;
    }

    auto converted = converter.convert(frame);
    REQUIRE(converted.has_value());
    const auto& result = *converted;
    const auto* nv12 = static_cast<const uint8_t*>(result.texture_handle);
    const size_t uv_offset = static_cast<size_t>(result.cpu_nv12_storage()->y_stride) * 4;
    REQUIRE(nv12[uv_offset + 0] == 100);
    REQUIRE(nv12[uv_offset + 1] == 200);
    REQUIRE(nv12[uv_offset + 2] == 110);
    REQUIRE(nv12[uv_offset + 3] == 210);

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

    auto converted = converter.convert(frame);
    REQUIRE(converted.has_value());
    TextureFrame result = std::move(*converted);
    REQUIRE(result.pts_us == 123456);
    REQUIRE(result.texture_handle != nullptr);
    REQUIRE(result.storage_kind() == FrameStorageKind::CpuPlanarYuv);
    REQUIRE(result.cpu_planar_yuv_storage() != nullptr);

    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: software conversion follows dynamic frame geometry",
          "[frame_converter]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(64, 64, AV_PIX_FMT_YUV420P));

    AVFrame* first = make_yuv420_frame(64, 64, 1000);
    auto first_converted = converter.convert(first);
    REQUIRE(first_converted.has_value());
    TextureFrame first_result = std::move(*first_converted);
    REQUIRE(first_result.texture_handle != nullptr);
    REQUIRE(first_result.width == 64);
    REQUIRE(first_result.height == 64);
    REQUIRE(first_result.cpu_planar_yuv_storage() != nullptr);
    REQUIRE(first_result.cpu_planar_yuv_storage()->plane_widths[0] == 64);
    REQUIRE(first_result.cpu_planar_yuv_storage()->plane_heights[0] == 64);
    REQUIRE(first_result.cpu_planar_yuv_storage()->plane_widths[1] == 32);
    REQUIRE(first_result.cpu_planar_yuv_storage()->plane_heights[1] == 32);

    AVFrame* second = make_yuv420_frame(96, 72, 2000);
    auto second_converted = converter.convert(second);
    REQUIRE(second_converted.has_value());
    TextureFrame second_result = std::move(*second_converted);
    REQUIRE(second_result.texture_handle != nullptr);
    REQUIRE(second_result.width == 96);
    REQUIRE(second_result.height == 72);
    REQUIRE(second_result.cpu_planar_yuv_storage() != nullptr);
    REQUIRE(second_result.cpu_planar_yuv_storage()->plane_widths[0] == 96);
    REQUIRE(second_result.cpu_planar_yuv_storage()->plane_heights[0] == 72);
    REQUIRE(second_result.cpu_planar_yuv_storage()->plane_widths[1] == 48);
    REQUIRE(second_result.cpu_planar_yuv_storage()->plane_heights[1] == 36);
    REQUIRE(second_result.cpu_planar_yuv_storage()->planes[0] !=
            first_result.cpu_planar_yuv_storage()->planes[0]);

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

    auto converted = converter.convert(frame);
    REQUIRE(converted.has_value());
    TextureFrame result = std::move(*converted);
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

    auto converted = converter.convert(frame);
    REQUIRE(converted.has_value());
    TextureFrame result = std::move(*converted);
    REQUIRE(result.texture_handle != nullptr);
    REQUIRE_FALSE(result.is_nv12);
    REQUIRE_FALSE(result.is_p010);
    REQUIRE(result.cpu_planar_yuv_storage() != nullptr);
    REQUIRE(result.cpu_planar_yuv_storage()->bit_depth == 10);
    REQUIRE(result.cpu_planar_yuv_storage()->bytes_per_sample == 2);
    REQUIRE(result.cpu_planar_yuv_storage()->plane_layout ==
            CpuYuvPlaneLayout::PlanarYuv420);
    REQUIRE(result.color.range == VIDEO_COLOR_RANGE_LIMITED);
    REQUIRE(result.color.matrix == VIDEO_COLOR_MATRIX_BT2020_NCL);
    REQUIRE(result.color.transfer == VIDEO_COLOR_TRANSFER_PQ);
    REQUIRE(result.color.primaries == VIDEO_COLOR_PRIMARIES_BT2020);

    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: preserves YUV420P10LE as CPU planar 10-bit before shader",
          "[frame_converter][color]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(4, 4, AV_PIX_FMT_YUV420P10LE));

    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_YUV420P10LE;
    frame->width = 4;
    frame->height = 4;
    REQUIRE(av_frame_get_buffer(frame, 0) >= 0);

    for (int y = 0; y < 4; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(frame->data[0] + y * frame->linesize[0]);
        for (int x = 0; x < 4; ++x) {
            row[x] = 512;
        }
    }
    for (int y = 0; y < 2; ++y) {
        auto* u = reinterpret_cast<uint16_t*>(frame->data[1] + y * frame->linesize[1]);
        auto* v = reinterpret_cast<uint16_t*>(frame->data[2] + y * frame->linesize[2]);
        for (int x = 0; x < 2; ++x) {
            u[x] = 256;
            v[x] = 768;
        }
    }

    auto converted = converter.convert(frame);
    REQUIRE(converted.has_value());
    TextureFrame result = std::move(*converted);
    REQUIRE_FALSE(result.is_nv12);
    REQUIRE_FALSE(result.is_p010);
    REQUIRE(result.cpu_planar_yuv_storage() != nullptr);
    const auto* planar = result.cpu_planar_yuv_storage();
    REQUIRE(planar->bit_depth == 10);
    REQUIRE(planar->bytes_per_sample == 2);
    REQUIRE(planar->plane_layout == CpuYuvPlaneLayout::PlanarYuv420);
    REQUIRE(planar->sample_alignment == CpuYuvSampleAlignment::Packed);
    REQUIRE(planar->strides[0] >= 8);
    REQUIRE(planar->strides[1] >= 4);
    REQUIRE(planar->strides[2] >= 4);

    const auto* y_plane = reinterpret_cast<const uint16_t*>(planar->planes[0]);
    const auto* u_plane = reinterpret_cast<const uint16_t*>(planar->planes[1]);
    const auto* v_plane = reinterpret_cast<const uint16_t*>(planar->planes[2]);
    REQUIRE(y_plane[0] == static_cast<uint16_t>(512u));
    REQUIRE(u_plane[0] == static_cast<uint16_t>(256u));
    REQUIRE(v_plane[0] == static_cast<uint16_t>(768u));

    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: YUVJ444P defaults to full range when metadata is absent",
          "[frame_converter][color]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(4, 4, AV_PIX_FMT_YUVJ444P));

    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_YUVJ444P;
    frame->width = 4;
    frame->height = 4;
    REQUIRE(av_frame_get_buffer(frame, 0) >= 0);
    for (int p = 0; p < 3; ++p) {
        for (int y = 0; y < 4; ++y) {
            memset(frame->data[p] + y * frame->linesize[p], p == 0 ? 64 : 128, 4);
        }
    }

    auto converted = converter.convert(frame);
    REQUIRE(converted.has_value());
    REQUIRE(converted->color.range == VIDEO_COLOR_RANGE_FULL);

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

TEST_CASE("FrameConverter: rejects oversized software frame geometry",
          "[frame_converter]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(20000, 64, AV_PIX_FMT_YUV420P) == false);

    REQUIRE(converter.init_software(64, 64, AV_PIX_FMT_YUV420P));
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 20000;
    frame->height = 64;
    frame->pts = 5000;

    auto result = converter.convert(frame);
    REQUIRE(result.has_value() == false);

    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: unsupported software format returns no frame",
          "[frame_converter]") {
    FrameConverter converter;
    REQUIRE(converter.init_software(0, 0, AV_PIX_FMT_NONE));

    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = 64;
    frame->height = 64;
    frame->pts = 6000;
    REQUIRE(av_frame_get_buffer(frame, 0) >= 0);

    auto result = converter.convert(frame);
    REQUIRE(result.has_value() == false);

    av_frame_free(&frame);
}

TEST_CASE("FrameConverter: direct D3D11 hardware frame keeps AVFrame ownership",
          "[frame_converter][hw][av_frame_lifetime]") {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        &feature_level,
        1,
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        nullptr)));

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 2;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));

    std::recursive_mutex device_mutex;
    FrameConverter converter;
    REQUIRE(converter.init_hardware(
        nullptr,
        nullptr,
        64,
        64,
        HwDecodeType::D3D11VA,
        false,
        &device_mutex));

    std::atomic<int> free_count{0};
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_D3D11;
    frame->width = 64;
    frame->height = 64;
    frame->pts = 7000;
    frame->data[0] = reinterpret_cast<uint8_t*>(texture.Get());
    frame->data[1] = reinterpret_cast<uint8_t*>(intptr_t{1});
    auto* token = static_cast<uint8_t*>(av_malloc(1));
    REQUIRE(token != nullptr);
    frame->buf[0] = av_buffer_create(
        token, 1, free_counted_buffer, &free_count, 0);
    REQUIRE(frame->buf[0] != nullptr);

    auto converted = converter.convert(frame);
    REQUIRE(converted.has_value());
    REQUIRE(converted->hw_frame_ref != nullptr);
    REQUIRE(converted->storage_kind() == FrameStorageKind::D3D11Nv12);
    REQUIRE(converted->texture_handle == texture.Get());
    REQUIRE(converted->texture_array_index == 1);

    av_frame_unref(frame);
    REQUIRE(free_count.load(std::memory_order_relaxed) == 0);

    converted.reset();
    REQUIRE(free_count.load(std::memory_order_relaxed) == 1);

    av_frame_free(&frame);
}

TEST_CASE("TextureFrame: storage exposes D3D11 NV12 texture metadata", "[frame_storage]") {
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
    REQUIRE(frame.storage_class() == FrameStorageClass::HardwareTexture);
    REQUIRE(frame.d3d11_nv12_storage() != nullptr);
    REQUIRE(frame.d3d11_nv12_storage()->texture == texture);
    REQUIRE(frame.d3d11_nv12_storage()->array_index == 7);
    REQUIRE(frame.d3d11_nv12_storage()->frame_ref == ref);
}

TEST_CASE("TextureFrame: storage exposes CPU NV12 metadata", "[frame_storage]") {
    TextureFrame frame;
    auto data = std::make_shared<std::vector<uint8_t>>(64 * 64 * 3 / 2);

    frame.texture_handle = data->data();
    frame.cpu_data = data;
    frame.is_nv12 = true;
    frame.storage = CpuNv12FrameStorage{data, 64, 64};

    REQUIRE(frame.storage_kind() == FrameStorageKind::CpuNv12);
    REQUIRE(frame.storage_class() == FrameStorageClass::CpuPixels);
    REQUIRE(frame.cpu_nv12_storage() != nullptr);
    REQUIRE(frame.cpu_nv12_storage()->data == data);
    REQUIRE(frame.cpu_nv12_storage()->y_stride == 64);
    REQUIRE(frame.cpu_nv12_storage()->uv_stride == 64);
}

TEST_CASE("TextureFrame: storage exposes CPU planar YUV metadata", "[frame_storage]") {
    TextureFrame frame;
    auto ref = std::shared_ptr<void>(reinterpret_cast<void*>(0x5678), [](void*) {});
    const uint8_t y = 16;
    const uint8_t u = 128;
    const uint8_t v = 129;

    frame.texture_handle = const_cast<uint8_t*>(&y);
    frame.storage = CpuPlanarYuvFrameStorage{
        ref,
        {&y, &u, &v},
        {64, 32, 32},
        {64, 32, 32},
        {64, 32, 32},
        1,
    };

    REQUIRE(frame.storage_kind() == FrameStorageKind::CpuPlanarYuv);
    REQUIRE(frame.storage_class() == FrameStorageClass::CpuPixels);
    REQUIRE(frame.cpu_planar_yuv_storage() != nullptr);
    REQUIRE(frame.cpu_planar_yuv_storage()->frame_ref == ref);
    REQUIRE(frame.cpu_planar_yuv_storage()->planes[0] == &y);
    REQUIRE(frame.cpu_planar_yuv_storage()->planes[1] == &u);
    REQUIRE(frame.cpu_planar_yuv_storage()->planes[2] == &v);
    REQUIRE(frame.cpu_planar_yuv_storage()->strides[0] == 64);
    REQUIRE(frame.cpu_planar_yuv_storage()->bytes_per_sample == 1);
}

TEST_CASE("TextureFrame: storage exposes CVPixelBuffer metadata", "[frame_storage]") {
    TextureFrame frame;
    auto ref = std::shared_ptr<void>(reinterpret_cast<void*>(0x2468), [](void*) {});
    void* pixel_buffer = reinterpret_cast<void*>(0x1357);

    frame.texture_handle = pixel_buffer;
    frame.is_ref = true;
    frame.is_nv12 = true;
    frame.hw_frame_ref = ref;
    frame.storage = MacOSCVPixelBufferFrameStorage{
        pixel_buffer,
        0x34323066u,
        2,
        true,
        1920,
        1080,
        ref,
    };

    REQUIRE(frame.storage_kind() == FrameStorageKind::MacOSCVPixelBuffer);
    REQUIRE(frame.storage_class() == FrameStorageClass::CVPixelBuffer);
    REQUIRE(frame.cv_pixel_buffer_storage() != nullptr);
    REQUIRE(frame.cv_pixel_buffer_storage()->pixel_buffer == pixel_buffer);
    REQUIRE(frame.cv_pixel_buffer_storage()->pixel_format == 0x34323066u);
    REQUIRE(frame.cv_pixel_buffer_storage()->plane_count == 2);
    REQUIRE(frame.cv_pixel_buffer_storage()->is_p010);
    REQUIRE(frame.cv_pixel_buffer_storage()->coded_width == 1920);
    REQUIRE(frame.cv_pixel_buffer_storage()->coded_height == 1080);
    REQUIRE(frame.cv_pixel_buffer_storage()->frame_ref == ref);
}
