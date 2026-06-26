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

    auto d3d_headless = valid_windowed_config();
    d3d_headless.headless = true;
    d3d_headless.hwnd = nullptr;
    d3d_headless.backend.type = vr::RendererBackendType::D3D11;
    d3d_headless.backend.adapter = reinterpret_cast<void*>(0x5678);
    if (!vr::validate_renderer_config(d3d_headless).ok) {
        return fail("valid D3D11 headless renderer config was rejected");
    }
    d3d_headless.backend.adapter = nullptr;
    if (vr::validate_renderer_config(d3d_headless).ok) {
        return fail("D3D11 headless renderer config without adapter was accepted");
    }

    auto wgpu_metal_headless = valid_windowed_config();
    wgpu_metal_headless.headless = true;
    wgpu_metal_headless.hwnd = nullptr;
    wgpu_metal_headless.backend.type = vr::RendererBackendType::WgpuMetal;
    wgpu_metal_headless.backend.output = reinterpret_cast<void*>(0x9abc);
    if (!vr::validate_renderer_config(wgpu_metal_headless).ok) {
        return fail("valid WgpuMetal headless renderer config was rejected");
    }
    wgpu_metal_headless.backend.output = nullptr;
    if (vr::validate_renderer_config(wgpu_metal_headless).ok) {
        return fail("WgpuMetal headless renderer config without output was accepted");
    }

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
