#include "viewport_capture_service.h"

#include "common/win_utf8.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <vector>

namespace {

struct CaptureStats {
    double avg_luma = 0.0;
    double non_black_ratio = 0.0;
};

std::string Fnv1a64Hex(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

CaptureStats ComputeCaptureStats(const std::vector<uint8_t>& bgra) {
    CaptureStats stats;
    const size_t pixel_count = bgra.size() / 4;
    if (pixel_count == 0) {
        return stats;
    }

    uint64_t luma_sum = 0;
    size_t non_black = 0;
    for (size_t i = 0; i < pixel_count; ++i) {
        const size_t off = i * 4;
        const uint8_t b = bgra[off + 0];
        const uint8_t g = bgra[off + 1];
        const uint8_t r = bgra[off + 2];
        const int luma = (77 * static_cast<int>(r) +
                          150 * static_cast<int>(g) +
                          29 * static_cast<int>(b)) >> 8;
        luma_sum += static_cast<uint64_t>(luma);
        if (r > 8 || g > 8 || b > 8) {
            ++non_black;
        }
    }

    stats.avg_luma = static_cast<double>(luma_sum) / static_cast<double>(pixel_count);
    stats.non_black_ratio = static_cast<double>(non_black) / static_cast<double>(pixel_count);
    return stats;
}

std::vector<uint8_t> CropAndResizeBgra(const std::vector<uint8_t>& bgra,
                                       int source_width,
                                       int source_height,
                                       int x,
                                       int y,
                                       int width,
                                       int height,
                                       int max_size,
                                       int& output_width,
                                       int& output_height) {
    output_width = 0;
    output_height = 0;
    if (bgra.empty() || source_width <= 0 || source_height <= 0 ||
        width <= 0 || height <= 0) {
        return {};
    }

    const int left = std::clamp(x, 0, source_width);
    const int top = std::clamp(y, 0, source_height);
    const int right = std::clamp(x + width, left, source_width);
    const int bottom = std::clamp(y + height, top, source_height);
    const int crop_width = right - left;
    const int crop_height = bottom - top;
    if (crop_width <= 0 || crop_height <= 0) {
        return {};
    }

    double scale = 1.0;
    if (max_size > 0) {
        scale = std::min(1.0,
                         static_cast<double>(max_size) /
                             static_cast<double>(std::max(crop_width, crop_height)));
    }
    output_width = std::max(1, static_cast<int>(crop_width * scale + 0.5));
    output_height = std::max(1, static_cast<int>(crop_height * scale + 0.5));

    std::vector<uint8_t> output(static_cast<size_t>(output_width) *
                                static_cast<size_t>(output_height) * 4);
    for (int oy = 0; oy < output_height; ++oy) {
        const int sy = top + std::min(crop_height - 1,
                                      static_cast<int>(
                                          static_cast<int64_t>(oy) * crop_height /
                                          output_height));
        for (int ox = 0; ox < output_width; ++ox) {
            const int sx = left + std::min(crop_width - 1,
                                           static_cast<int>(
                                               static_cast<int64_t>(ox) * crop_width /
                                               output_width));
            const size_t src = (static_cast<size_t>(sy) * source_width + sx) * 4;
            const size_t dst = (static_cast<size_t>(oy) * output_width + ox) * 4;
            output[dst + 0] = bgra[src + 0];
            output[dst + 1] = bgra[src + 1];
            output[dst + 2] = bgra[src + 2];
            output[dst + 3] = bgra[src + 3];
        }
    }
    return output;
}

bool SaveBgraToPng(const std::vector<uint8_t>& bgra,
                   int width,
                   int height,
                   const std::string& path) {
    if (bgra.empty() || width <= 0 || height <= 0 || path.empty()) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream) {
        return false;
    }

    const auto wide_path = vr::win_utf8::utf16_from_utf8(path);
    hr = stream->InitializeFromFilename(wide_path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr) || !encoder) {
        return false;
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr) || !frame) {
        return false;
    }
    hr = frame->Initialize(props.Get());
    if (FAILED(hr)) {
        return false;
    }
    hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    if (FAILED(hr)) {
        return false;
    }

    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&pixel_format);
    if (FAILED(hr)) {
        return false;
    }

    const UINT stride = static_cast<UINT>(width * 4);
    const UINT image_size = stride * static_cast<UINT>(height);
    hr = frame->WritePixels(
        static_cast<UINT>(height), stride, image_size,
        const_cast<BYTE*>(bgra.data()));
    if (FAILED(hr)) {
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr)) {
        return false;
    }
    hr = encoder->Commit();
    return SUCCEEDED(hr);
}

ViewportCaptureStatus BuildCaptureResult(std::vector<uint8_t> bgra,
                                         int width,
                                         int height,
                                         const std::string& output_path,
                                         ViewportCaptureResult& result) {
    result.width = width;
    result.height = height;
    result.hash = Fnv1a64Hex(bgra);
    const CaptureStats stats = ComputeCaptureStats(bgra);
    result.avg_luma = stats.avg_luma;
    result.non_black_ratio = stats.non_black_ratio;
    if (!output_path.empty() &&
        !SaveBgraToPng(bgra, result.width, result.height, output_path)) {
        return ViewportCaptureStatus::SaveFailed;
    }
    return ViewportCaptureStatus::Ok;
}

} // namespace

ViewportCaptureStatus ViewportCaptureService::Capture(vr::NativePlayer& player,
                                                      const std::string& output_path,
                                                      ViewportCaptureResult& result) const {
    result = {};
    result.output_path = output_path;

    std::vector<uint8_t> bgra;
    if (!player.capture_front_buffer(bgra, result.width, result.height)) {
        return ViewportCaptureStatus::CaptureFailed;
    }

    return BuildCaptureResult(
        std::move(bgra), result.width, result.height, output_path, result);
}

ViewportCaptureStatus ViewportCaptureService::CaptureRegion(
    vr::NativePlayer& player,
    int x,
    int y,
    int width,
    int height,
    int max_size,
    const std::string& output_path,
    ViewportCaptureResult& result) const {
    result = {};
    result.output_path = output_path;

    std::vector<uint8_t> bgra;
    int source_width = 0;
    int source_height = 0;
    if (!player.capture_front_buffer(bgra, source_width, source_height)) {
        return ViewportCaptureStatus::CaptureFailed;
    }

    int output_width = 0;
    int output_height = 0;
    auto region = CropAndResizeBgra(
        bgra, source_width, source_height, x, y, width, height, max_size,
        output_width, output_height);
    if (region.empty()) {
        return ViewportCaptureStatus::CaptureFailed;
    }
    return BuildCaptureResult(
        std::move(region), output_width, output_height, output_path, result);
}
