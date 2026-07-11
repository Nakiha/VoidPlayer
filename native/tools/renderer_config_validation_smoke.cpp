#include "renderer/renderer_config_validation.h"

#include <iostream>
#include <string>

namespace {

int fail(const char* message) {
    std::cerr << message << "\n";
    return 1;
}

vr::RendererConfig valid_windowed_config() {
    vr::RendererConfig config;
    config.video_paths = {"/tmp/video.mp4"};
    config.hwnd = reinterpret_cast<void*>(0x1234);
    config.width = 1920;
    config.height = 1080;
    return config;
}

} // namespace

int main() {
    if (!vr::validate_renderer_config(valid_windowed_config()).ok) {
        return fail("valid windowed renderer config was rejected");
    }

    auto d3d12_offscreen = valid_windowed_config();
    d3d12_offscreen.offscreen = true;
    d3d12_offscreen.hwnd = nullptr;
    d3d12_offscreen.backend.type = vr::RendererBackendType::NativeD3D12;
    d3d12_offscreen.backend.output = reinterpret_cast<void*>(0x5678);
    if (vr::validate_renderer_config(d3d12_offscreen).ok) {
        return fail("reserved NativeD3D12 offscreen renderer config was accepted");
    }

    auto metal_offscreen = valid_windowed_config();
    metal_offscreen.offscreen = true;
    metal_offscreen.hwnd = nullptr;
    metal_offscreen.backend.type = vr::RendererBackendType::Metal;
    metal_offscreen.backend.output = reinterpret_cast<void*>(0x9abc);
#ifdef __APPLE__
    if (!vr::validate_renderer_config(metal_offscreen).ok) {
        return fail("valid Metal offscreen renderer config was rejected");
    }
    metal_offscreen.backend.output = nullptr;
    if (vr::validate_renderer_config(metal_offscreen).ok) {
        return fail("Metal offscreen renderer config without output was accepted");
    }
#else
    if (vr::validate_renderer_config(metal_offscreen).ok) {
        return fail("non-Apple Metal offscreen renderer config was accepted");
    }
#endif

    auto invalid_size = valid_windowed_config();
    invalid_size.width = 0;
    if (vr::validate_renderer_config(invalid_size).ok) {
        return fail("renderer config with invalid width was accepted");
    }

    if (!vr::validate_playback_speed(1.0).ok ||
        vr::validate_playback_speed(0.0).ok) {
        return fail("playback speed validation failed");
    }

    if (!vr::validate_loop_range(true, 0, 1000).ok ||
        vr::validate_loop_range(true, 1000, 1000).ok) {
        return fail("loop range validation failed");
    }

    std::cout << "renderer config validation smoke passed\n";
    return 0;
}
