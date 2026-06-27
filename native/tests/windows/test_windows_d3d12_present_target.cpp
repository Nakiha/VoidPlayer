#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#include "test_utils.h"
#include "windows/presentation/windows_d3d12_present_target.h"
#include "windows/wgpu/wgpu_d3d12_ffi_bridge.h"

#include <array>
#include <d3d12.h>
#include <string>
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

    const float color[4] = {0.05f, 0.10f, 0.20f, 1.0f};
    INFO(target.last_error());
    REQUIRE(target.clear_and_present(color, 0));
#else
    SUCCEED("Windows D3D12 present target is Windows-only");
#endif
}
