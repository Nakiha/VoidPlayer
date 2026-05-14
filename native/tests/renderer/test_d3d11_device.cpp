#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/d3d11/device.h"
#include <wrl/client.h>

using namespace vr::test;

TEST_CASE("D3D11Device initialization", "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window();

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    REQUIRE(dev.device() != nullptr);
    REQUIRE(dev.context() != nullptr);
    REQUIRE(dev.feature_level() == D3D_FEATURE_LEVEL_11_0);
    REQUIRE(dev.device_lost() == false);
    REQUIRE(dev.device_removed_reason() == S_OK);
    REQUIRE(dev.poll_device_removed("test") == false);

    destroy_window(hwnd);
}

TEST_CASE("D3D11Device swap chain is created", "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window();

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    REQUIRE(dev.swap_chain() != nullptr);

    DXGI_SWAP_CHAIN_DESC desc = {};
    REQUIRE(SUCCEEDED(dev.swap_chain()->GetDesc(&desc)));
    REQUIRE(desc.BufferCount == 2);
    REQUIRE(desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);

    destroy_window(hwnd);
}

TEST_CASE("D3D11Device present does not crash", "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window();

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    REQUIRE(dev.present(0) == true);

    destroy_window(hwnd);
}

TEST_CASE("D3D11Device records device-lost failures", "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window();

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    REQUIRE(dev.record_device_error("test", E_INVALIDARG) == false);
    REQUIRE(dev.device_lost() == false);

    REQUIRE(dev.record_device_error("test", DXGI_ERROR_DEVICE_REMOVED) == true);
    REQUIRE(dev.device_lost() == true);
    REQUIRE(dev.device_removed_reason() == DXGI_ERROR_DEVICE_REMOVED);

    destroy_window(hwnd);
}

TEST_CASE("D3D11Device shutdown clears all pointers", "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window();

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    dev.shutdown();

    REQUIRE(dev.device() == nullptr);
    REQUIRE(dev.context() == nullptr);
    REQUIRE(dev.swap_chain() == nullptr);
    REQUIRE(dev.feature_level() == static_cast<D3D_FEATURE_LEVEL>(0));

    destroy_window(hwnd);
}

TEST_CASE("D3D11Device shutdown clears immediate-context bindings",
          "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window();

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context = dev.context();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(dev.device()->CreateTexture2D(&desc, nullptr, &texture)));

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    REQUIRE(SUCCEEDED(dev.device()->CreateRenderTargetView(
        texture.Get(), nullptr, &rtv)));

    ID3D11RenderTargetView* bound_rtv = rtv.Get();
    context->OMSetRenderTargets(1, &bound_rtv, nullptr);
    rtv.Reset();

    dev.shutdown();

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> still_bound;
    context->OMGetRenderTargets(1, still_bound.GetAddressOf(), nullptr);
    REQUIRE(still_bound.Get() == nullptr);

    context.Reset();
    destroy_window(hwnd);
}

TEST_CASE("D3D11Device resize to 1920x1080", "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window(1920, 1080);

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    REQUIRE(dev.resize(1920, 1080) == true);

    // Present after resize should also work
    REQUIRE(dev.present(0) == true);

    destroy_window(hwnd);
}
