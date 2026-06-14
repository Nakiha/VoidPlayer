#pragma once

#include "common/logging.h"
#include "renderer/color/color_strategy.h"
#include "renderer/render/backend_type.h"

#include <string>
#include <vector>

namespace vr {

using RendererBackendType = RenderBackendKind;
class PresentationBackendProvider;

/// Platform-specific renderer interop values.
/// D3D11 uses `adapter` as the Flutter Windows DXGI adapter pointer.
/// Backends that present into a host-owned target, such as macOS Metal writing
/// into a CVPixelBuffer, use `output`.
struct RendererBackendInterop {
    RendererBackendType type = RendererBackendType::D3D11;
    void* adapter = nullptr;
    void* output = nullptr;
    int max_track_slots = 0;
    const PresentationBackendProvider* provider = nullptr;
    ColorOutputTarget output_target = ColorOutputTarget::kSDRToneMappedBT709;
    double sdr_white_level_nits = 80.0;
};

struct RendererConfig {
    std::vector<std::string> video_paths;
    void* hwnd = nullptr;
    int width = 1920;
    int height = 1080;
    bool use_hardware_decode = true;
    int initial_file_id = 1;

    /// Headless mode: render to offscreen texture instead of swap chain.
    bool headless = false;

    /// Native backend interop for headless mode.
    RendererBackendInterop backend;

    /// Logging configuration. Applied during initialize().
    /// Can also be set independently via configure_logging() before init.
    LogConfig log_config;
};

} // namespace vr
