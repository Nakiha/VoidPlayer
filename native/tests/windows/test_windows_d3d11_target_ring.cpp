#include <catch2/catch_test_macros.hpp>

#include "windows/presentation/windows_d3d11_target_ring.h"

#include <array>

using namespace vr;

namespace {

Microsoft::WRL::ComPtr<ID3D11Device> create_warp_device() {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  const HRESULT result = D3D11CreateDevice(
      nullptr,
      D3D_DRIVER_TYPE_WARP,
      nullptr,
      0,
      levels,
      1,
      D3D11_SDK_VERSION,
      &device,
      nullptr,
      &context);
  REQUIRE(SUCCEEDED(result));
  return device;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> create_target(
    ID3D11Device* device,
    int width = 64,
    int height = 32,
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
  return texture;
}

}  // namespace

TEST_CASE("Windows D3D11 target ring preserves displayed and protected targets",
          "[windows_d3d11_target_ring]") {
  auto device = create_warp_device();
  std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 3> textures = {
      create_target(device.Get()),
      create_target(device.Get()),
      create_target(device.Get()),
  };
  const void* raw[] = {
      textures[0].Get(), textures[1].Get(), textures[2].Get()};
  std::string error;
  WindowsD3D11TargetRing ring;
  REQUIRE(ring.install(raw,
                       std::size(raw),
                       textures[0].Get(),
                       textures[1].Get(),
                       64,
                       32,
                       DXGI_FORMAT_B8G8R8A8_UNORM,
                       error));
  REQUIRE(error.empty());
  REQUIRE(ring.installed());
  REQUIRE(ring.device().Get() == device.Get());
  REQUIRE(ring.state_for_test(textures[0].Get()) ==
          WindowsD3D11TargetState::Displayed);
  REQUIRE(ring.state_for_test(textures[1].Get()) ==
          WindowsD3D11TargetState::Protected);

  auto draw_target = ring.acquire_draw_target();
  REQUIRE(draw_target.Get() == textures[2].Get());
  REQUIRE_FALSE(ring.acquire_draw_target());
  REQUIRE(ring.complete_draw_target(draw_target.Get(), true));
  REQUIRE(ring.state_for_test(draw_target.Get()) ==
          WindowsD3D11TargetState::Completed);

  ring.mark_displayed(draw_target.Get());
  REQUIRE(ring.state_for_test(draw_target.Get()) ==
          WindowsD3D11TargetState::Displayed);
  ring.release(draw_target.Get());
  REQUIRE(ring.state_for_test(draw_target.Get()) ==
          WindowsD3D11TargetState::Displayed);
  REQUIRE(ring.state_for_test(textures[0].Get()) ==
          WindowsD3D11TargetState::Available);

  const auto diagnostics = ring.diagnostics();
  REQUIRE(diagnostics.target_count == 3);
  REQUIRE(diagnostics.acquisition_count == 1);
  REQUIRE(diagnostics.completion_count == 1);
  REQUIRE(diagnostics.backpressure_count == 1);
  REQUIRE(diagnostics.width == 64);
  REQUIRE(diagnostics.height == 32);
}

TEST_CASE("Windows D3D11 target ring rejects malformed viewport targets",
          "[windows_d3d11_target_ring]") {
  auto device = create_warp_device();
  auto first = create_target(device.Get());
  auto second = create_target(device.Get());
  auto wrong_size = create_target(device.Get(), 63, 32);
  const void* too_few[] = {first.Get(), second.Get()};
  const void* wrong_geometry[] = {first.Get(), second.Get(), wrong_size.Get()};
  std::string error;
  WindowsD3D11TargetRing ring;
  REQUIRE_FALSE(ring.install(too_few,
                             std::size(too_few),
                             nullptr,
                             nullptr,
                             64,
                             32,
                             DXGI_FORMAT_B8G8R8A8_UNORM,
                             error));
  REQUIRE_FALSE(error.empty());
  REQUIRE_FALSE(ring.install(wrong_geometry,
                             std::size(wrong_geometry),
                             nullptr,
                             nullptr,
                             64,
                             32,
                             DXGI_FORMAT_B8G8R8A8_UNORM,
                             error));
  REQUIRE_FALSE(error.empty());
}
