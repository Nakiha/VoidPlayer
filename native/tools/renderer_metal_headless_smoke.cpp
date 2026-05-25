#include "tools/test_video_assets.h"
#include "video_renderer/renderer.h"

#include <CoreVideo/CoreVideo.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifndef VIDEO_TEST_DIR
#define VIDEO_TEST_DIR ""
#endif

namespace {

struct PixelBufferHolder {
    CVPixelBufferRef buffer = nullptr;

    ~PixelBufferHolder() {
        if (buffer) {
            CVPixelBufferRelease(buffer);
        }
    }
};

PixelBufferHolder make_bgra_pixel_buffer(int width, int height) {
    PixelBufferHolder holder;
    const void* keys[] = {kCVPixelBufferMetalCompatibilityKey};
    const void* values[] = {kCFBooleanTrue};
    CFDictionaryRef attrs = CFDictionaryCreate(
        kCFAllocatorDefault,
        keys,
        values,
        1,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferCreate(
        kCFAllocatorDefault,
        width,
        height,
        kCVPixelFormatType_32BGRA,
        attrs,
        &holder.buffer);
    if (attrs) {
        CFRelease(attrs);
    }
    return holder;
}

double non_black_ratio_pixel_buffer(CVPixelBufferRef buffer) {
    if (!buffer ||
        CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return 0.0;
    }
    const int width = static_cast<int>(CVPixelBufferGetWidth(buffer));
    const int height = static_cast<int>(CVPixelBufferGetHeight(buffer));
    const int stride = static_cast<int>(CVPixelBufferGetBytesPerRow(buffer));
    const auto* bgra = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    size_t non_black = 0;
    size_t pixels = 0;
    if (bgra && width > 0 && height > 0 && stride >= width * 4) {
        for (int y = 0; y < height; ++y) {
            const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
            for (int x = 0; x < width; ++x) {
                const uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
                if (pixel[0] > 4 || pixel[1] > 4 || pixel[2] > 4) {
                    ++non_black;
                }
                ++pixels;
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return pixels == 0 ? 0.0 : static_cast<double>(non_black) / static_cast<double>(pixels);
}

} // namespace

int main() {
    const std::string path = vp_tools::h264_smoke_video_path(VIDEO_TEST_DIR);
    if (path.empty()) {
        std::cerr << "missing h264 smoke video\n";
        return 1;
    }

    constexpr int target_width = 1920;
    constexpr int target_height = 1080;
    auto target = make_bgra_pixel_buffer(target_width, target_height);
    if (!target.buffer) {
        std::cerr << "failed to create Metal-compatible target buffer\n";
        return 1;
    }

    vr::Renderer renderer;
    vr::RendererConfig config;
    config.video_paths = {path};
    config.width = target_width;
    config.height = target_height;
    config.headless = true;
    config.use_hardware_decode = true;
    config.backend.type = vr::RendererBackendType::Metal;
    config.backend.output = target.buffer;
    config.backend.max_track_slots = 1;

    if (!renderer.initialize(config)) {
        std::cerr << "shared Renderer failed to initialize with Metal backend\n";
        return 1;
    }

    double non_black = 0.0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        non_black = non_black_ratio_pixel_buffer(target.buffer);
        if (non_black > 0.5) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    renderer.shutdown();

    if (non_black <= 0.5) {
        std::cerr << "shared Renderer Metal target did not receive a visible frame; non_black="
                  << non_black << "\n";
        return 1;
    }

    std::cout << "shared Renderer Metal headless smoke passed; non_black="
              << non_black << "\n";
    return 0;
}
