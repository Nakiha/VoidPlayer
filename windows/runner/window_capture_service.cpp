#include "window_capture_service.h"

#include "common/win_utf8.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <iomanip>
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

bool CaptureWindowBgra(HWND requested_window,
                       std::vector<uint8_t>& bgra,
                       int& width,
                       int& height) {
    HWND window = GetAncestor(requested_window, GA_ROOT);
    if (!window) {
        window = requested_window;
    }
    if (!IsWindow(window) || IsIconic(window)) {
        return false;
    }

    RECT rect = {};
    if (!GetWindowRect(window, &rect)) {
        return false;
    }
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) {
        return false;
    }
    HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (!memory_dc) {
        ReleaseDC(nullptr, screen_dc);
        return false;
    }
    HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, width, height);
    if (!bitmap) {
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
    const BOOL copied = BitBlt(
        memory_dc, 0, 0, width, height, screen_dc, rect.left, rect.top, SRCCOPY);
    SelectObject(memory_dc, old_bitmap);
    ReleaseDC(nullptr, screen_dc);
    if (!copied) {
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        return false;
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    bgra.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const int rows = GetDIBits(
        memory_dc, bitmap, 0, static_cast<UINT>(height), bgra.data(), &info,
        DIB_RGB_COLORS);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    if (rows != height) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }

    for (size_t i = 3; i < bgra.size(); i += 4) {
        bgra[i] = 255;
    }
    return true;
}

} // namespace

WindowCaptureStatus WindowCaptureService::Capture(
    HWND window,
    const std::string& output_path,
    WindowCaptureResult& result) const {
    result = {};
    result.output_path = output_path;

    if (!window || !IsWindow(window)) {
        return WindowCaptureStatus::InvalidWindow;
    }

    std::vector<uint8_t> bgra;
    if (!CaptureWindowBgra(window, bgra, result.width, result.height)) {
        return WindowCaptureStatus::CaptureFailed;
    }

    result.hash = Fnv1a64Hex(bgra);
    const CaptureStats stats = ComputeCaptureStats(bgra);
    result.avg_luma = stats.avg_luma;
    result.non_black_ratio = stats.non_black_ratio;
    if (!output_path.empty() &&
        !SaveBgraToPng(bgra, result.width, result.height, output_path)) {
        return WindowCaptureStatus::SaveFailed;
    }
    return WindowCaptureStatus::Ok;
}
