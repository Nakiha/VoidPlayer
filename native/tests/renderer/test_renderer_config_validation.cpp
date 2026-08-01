#include <catch2/catch_test_macros.hpp>

#include "renderer/decode/hw/hw_decode_provider.h"
#include "renderer/render/presentation_backend_factory.h"
#include "renderer/renderer_config_validation.h"

#include <string>

using namespace vr;

namespace {

RendererConfig valid_windowed_config() {
    RendererConfig config;
    config.video_paths = {"C:/video.mp4"};
    config.hwnd = reinterpret_cast<void*>(0x1234);
    config.width = 1920;
    config.height = 1080;
    config.use_hardware_decode = true;
    return config;
}

} // namespace

TEST_CASE("Renderer config validation accepts valid windowed config",
          "[renderer_config]") {
    REQUIRE(validate_renderer_config(valid_windowed_config()).ok);
}

TEST_CASE("Renderer default backend follows native platform presentation",
          "[renderer_config][presentation_backend]") {
    const auto default_backend = default_render_backend_kind();

#ifdef _WIN32
    REQUIRE(default_backend == RenderBackendKind::NativeD3D11);
    auto backend = create_presentation_backend(default_backend);
    REQUIRE(backend != nullptr);
    REQUIRE(backend->kind() == PresentationBackendKind::NativeD3D11);
#elif defined(__APPLE__)
    REQUIRE(default_backend == RenderBackendKind::Metal);
    auto backend = create_presentation_backend(default_backend);
    REQUIRE(backend != nullptr);
    REQUIRE(backend->kind() == PresentationBackendKind::Metal);
#else
    REQUIRE(default_backend == RenderBackendKind::Unknown);
    REQUIRE(create_presentation_backend(default_backend) == nullptr);
#endif
}

TEST_CASE("Renderer config validation rejects invalid dimensions",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.width = 0;
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.height = kMaxRendererDimension + 1;
    REQUIRE_FALSE(validate_renderer_config(config).ok);
}

TEST_CASE("Renderer config validation rejects invalid path lists",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.video_paths.clear();
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.video_paths = {""};
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.video_paths.assign(kMaxRendererVideoPaths + 1, "C:/video.mp4");
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.video_paths = {std::string("\xC3\x28", 2)};
    REQUIRE_FALSE(validate_renderer_config(config).ok);
}

TEST_CASE("Renderer config validation accepts Metal offscreen output on macOS",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.offscreen = true;
    config.hwnd = nullptr;
    config.backend.type = RendererBackendType::Metal;
    config.backend.output = reinterpret_cast<void*>(0x9abc);

#ifdef __APPLE__
    REQUIRE(validate_renderer_config(config).ok);

    config.backend.output = nullptr;
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config.backend.output = reinterpret_cast<void*>(0x9abc);
    config.backend.max_track_slots = kMaxRendererVideoPaths + 1;
    REQUIRE_FALSE(validate_renderer_config(config).ok);
#else
    REQUIRE_FALSE(validate_renderer_config(config).ok);
#endif
}

TEST_CASE("Windows native D3D11 backend requires the runner target contract",
          "[renderer_config][presentation_backend]") {
    auto config = valid_windowed_config();
    config.offscreen = true;
    config.hwnd = nullptr;
    config.backend.type = RendererBackendType::NativeD3D11;
    config.backend.output = reinterpret_cast<void*>(0x9abc);
    config.backend.max_track_slots = 1;

#ifdef _WIN32
    REQUIRE(validate_renderer_config(config).ok);
    REQUIRE(create_presentation_backend(RenderBackendKind::NativeD3D11) != nullptr);
#else
    REQUIRE_FALSE(validate_renderer_config(config).ok);
    REQUIRE(create_presentation_backend(RenderBackendKind::NativeD3D11) == nullptr);
#endif

    config.backend.output = nullptr;
    REQUIRE_FALSE(validate_renderer_config(config).ok);
    config.backend.type = RendererBackendType::NativeD3D12;
    config.backend.output = reinterpret_cast<void*>(0x9abc);
    REQUIRE_FALSE(validate_renderer_config(config).ok);
    REQUIRE(create_presentation_backend(RenderBackendKind::NativeD3D12) == nullptr);
}

TEST_CASE("Hardware decode compatibility follows active native backends",
          "[renderer_config][hw_decode]") {
    const auto d3d11_names = compatible_hw_decode_provider_names(
        RenderBackendKind::NativeD3D11,
        DecodeDeviceMode::IndependentDevice);
#ifdef _WIN32
    REQUIRE(d3d11_names.size() == 1);
    REQUIRE(std::string(d3d11_names.front()) == "D3D11VA");
#else
    REQUIRE(d3d11_names.empty());
#endif

    const auto d3d12_names = compatible_hw_decode_provider_names(
        RenderBackendKind::NativeD3D12,
        DecodeDeviceMode::IndependentDevice);
    REQUIRE(d3d12_names.empty());

    const auto d3d11_hwdownload_names = compatible_hw_decode_provider_names(
        RenderBackendKind::NativeD3D11,
        DecodeDeviceMode::FfmpegOwnedHwDownloadDevice);
#ifdef _WIN32
    REQUIRE(d3d11_hwdownload_names.size() == 1);
    REQUIRE(std::string(d3d11_hwdownload_names.front()) == "D3D11VA");
#else
    REQUIRE(d3d11_hwdownload_names.empty());
#endif

    const auto metal_hwdownload_names = compatible_hw_decode_provider_names(
        RenderBackendKind::Metal,
        DecodeDeviceMode::FfmpegOwnedHwDownloadDevice);
#ifdef __APPLE__
    bool has_videotoolbox = false;
    for (const char* name : metal_hwdownload_names) {
        has_videotoolbox =
            has_videotoolbox || std::string(name) == "VideoToolbox";
    }
    REQUIRE(has_videotoolbox);
#else
    REQUIRE(metal_hwdownload_names.empty());
#endif
}
