#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#include "test_utils.h"
#include "windows/presentation/windows_d3d12_present_target.h"
#include "windows/wgpu/wgpu_d3d12_ffi_bridge.h"

#include <array>
#include <d3d12.h>
#include <string>
#include <vector>
#endif

TEST_CASE("Windows D3D12 present target exposes a DComp back buffer",
          "[windows][presentation][d3d12_present]") {
#ifdef _WIN32
    std::array<char, 512> error{};
    VPWgpuD3D12Renderer* renderer =
        VPWgpuD3D12RendererCreate(error.data(), error.size());
    if (!renderer) {
        SKIP(std::string("wgpu-d3d12 renderer unavailable: ") + error.data());
    }
    struct RendererGuard {
        VPWgpuD3D12Renderer* renderer = nullptr;
        ~RendererGuard() {
            if (renderer) {
                VPWgpuD3D12RendererDestroy(renderer);
            }
        }
    } guard{renderer};

    auto* device = static_cast<ID3D12Device*>(
        VPWgpuD3D12RendererD3D12Device(renderer));
    auto* queue = static_cast<ID3D12CommandQueue*>(
        VPWgpuD3D12RendererD3D12CommandQueue(renderer));
    REQUIRE(device != nullptr);
    REQUIRE(queue != nullptr);

    HWND hwnd = vr::test::create_hidden_window(320, 180);
    REQUIRE(hwnd != nullptr);
    struct WindowGuard {
        HWND hwnd = nullptr;
        ~WindowGuard() { vr::test::destroy_window(hwnd); }
    } window_guard{hwnd};

    vr::WindowsD3D12PresentTarget target;
    if (!target.initialize(hwnd,
                           device,
                           queue,
                           320,
                           180,
                           vr::WindowsD3D12PresentTargetFormat::SDR)) {
        SKIP(std::string("D3D12 present target unavailable: ") +
             target.last_error());
    }
    REQUIRE(target.active());
    REQUIRE(target.width() == 320);
    REQUIRE(target.height() == 180);
    REQUIRE(target.dxgi_format() == DXGI_FORMAT_B8G8R8A8_UNORM);

    vr::WindowsD3D12PresentTargetFrame frame;
    INFO(target.last_error());
    REQUIRE(target.acquire_frame(frame));
    REQUIRE(frame.resource != nullptr);
    REQUIRE(frame.buffer_index < 3);
    REQUIRE(frame.width == 320);
    REQUIRE(frame.height == 180);
    REQUIRE(frame.dxgi_format == DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(frame.color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);

    std::array<char, 512> clear_error{};
    VPWgpuD3D12RenderTargetClearRequest clear_request = {};
    clear_request.d3d12_resource = frame.resource.Get();
    clear_request.format = VP_WGPU_D3D12_TEXTURE_FORMAT_BGRA8_UNORM;
    clear_request.width = frame.width;
    clear_request.height = frame.height;
    clear_request.color[0] = 0.05f;
    clear_request.color[1] = 0.10f;
    clear_request.color[2] = 0.20f;
    clear_request.color[3] = 1.0f;
    clear_request.error = clear_error.data();
    clear_request.error_size = clear_error.size();
    CAPTURE(clear_error.data());
    REQUIRE(VPWgpuD3D12RendererClearRenderTargetForProbe(
                renderer, &clear_request) == 0);
    REQUIRE(target.present(0));

    constexpr uint32_t kSourceWidth = 64;
    constexpr uint32_t kSourceHeight = 64;
    std::vector<uint8_t> y_plane(kSourceWidth * kSourceHeight, 180);
    std::vector<uint8_t> uv_plane((kSourceWidth / 2) * (kSourceHeight / 2) * 2,
                                  128);
    VPWgpuD3D12PresentDecisionInfo decision = {};
    decision.should_present = 1;
    decision.frame_count = 1;
    decision.track_count = 1;
    decision.background_color[3] = 1.0f;
    for (int i = 0; i < 4; ++i) {
        decision.order[i] = i;
        decision.inv_display_size_x[i] = 1.0f;
        decision.inv_display_size_y[i] = 1.0f;
        decision.source_width[i] = 1;
        decision.source_height[i] = 1;
        decision.coded_width[i] = 1;
        decision.coded_height[i] = 1;
        decision.nv12_uv_scale_x[i] = 1.0f;
        decision.nv12_uv_scale_y[i] = 1.0f;
        decision.color_range[i] = 1;
        decision.color_matrix[i] = 2;
        decision.color_transfer[i] = 1;
        decision.color_primaries[i] = 2;
    }
    decision.frames[0].present = 1;
    decision.frames[0].slot = 0;
    decision.frames[0].width = static_cast<int32_t>(kSourceWidth);
    decision.frames[0].height = static_cast<int32_t>(kSourceHeight);
    decision.source_width[0] = static_cast<int32_t>(kSourceWidth);
    decision.source_height[0] = static_cast<int32_t>(kSourceHeight);
    decision.coded_width[0] = static_cast<int32_t>(kSourceWidth);
    decision.coded_height[0] = static_cast<int32_t>(kSourceHeight);
    decision.yuv_format[0] = VP_WGPU_D3D12_TEXTURE_FORMAT_NV12;
    decision.y_stride[0] = static_cast<int32_t>(kSourceWidth);
    decision.uv_stride[0] = static_cast<int32_t>(kSourceWidth);

    vr::WindowsD3D12PresentTargetFrame composite_frame;
    REQUIRE(target.acquire_frame(composite_frame));
    std::array<char, 512> composite_error{};
    VPWgpuD3D12CompositeRequest composite = {};
    composite.destination_resource = composite_frame.resource.Get();
    composite.output_format = VP_WGPU_D3D12_TEXTURE_FORMAT_BGRA8_UNORM;
    composite.output_color_mode = VP_WGPU_D3D12_OUTPUT_COLOR_MODE_SDR;
    composite.source_formats[0] = VP_WGPU_D3D12_TEXTURE_FORMAT_NV12;
    composite.cpu_sources[0].y_data = y_plane.data();
    composite.cpu_sources[0].y_size = y_plane.size();
    composite.cpu_sources[0].uv_data = uv_plane.data();
    composite.cpu_sources[0].uv_size = uv_plane.size();
    composite.cpu_sources[0].format = VP_WGPU_D3D12_TEXTURE_FORMAT_NV12;
    composite.cpu_sources[0].y_stride = static_cast<int32_t>(kSourceWidth);
    composite.cpu_sources[0].uv_stride = static_cast<int32_t>(kSourceWidth);
    composite.cpu_sources[0].y_width = kSourceWidth;
    composite.cpu_sources[0].y_height = kSourceHeight;
    composite.cpu_sources[0].uv_width = kSourceWidth / 2;
    composite.cpu_sources[0].uv_height = kSourceHeight / 2;
    composite.decision = &decision;
    composite.width = static_cast<int32_t>(composite_frame.width);
    composite.height = static_cast<int32_t>(composite_frame.height);
    composite.error = composite_error.data();
    composite.error_size = composite_error.size();
    CAPTURE(composite_error.data());
    REQUIRE(VPWgpuD3D12RendererRenderComposite(renderer, &composite) == 0);
    REQUIRE(target.present(0));
    frame.resource.Reset();
    composite_frame.resource.Reset();

    REQUIRE(target.resize(
        384, 216, vr::WindowsD3D12PresentTargetFormat::SDR));
    REQUIRE(target.active());
    REQUIRE(target.width() == 384);
    REQUIRE(target.height() == 216);
    REQUIRE(target.dxgi_format() == DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(target.set_client_size(320, 180));
    REQUIRE(target.set_client_size(384, 216));
    vr::WindowsD3D12PresentTargetFrame resized_frame;
    REQUIRE(target.acquire_frame(resized_frame));
    REQUIRE(resized_frame.resource != nullptr);
    REQUIRE(resized_frame.width == 384);
    REQUIRE(resized_frame.height == 216);
    REQUIRE(resized_frame.dxgi_format == DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(target.present(0));

    const float color[4] = {0.05f, 0.10f, 0.20f, 1.0f};
    INFO(target.last_error());
    REQUIRE(target.clear_and_present(color, 0));
#else
    SUCCEED("Windows D3D12 present target is Windows-only");
#endif
}
