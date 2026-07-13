#include <catch2/catch_test_macros.hpp>

#include "renderer/render/presentation_backend_factory.h"
#include "windows/presentation/windows_d3d11_target_ring.h"

#include <array>
#include <cmath>

using namespace vr;

namespace {

Microsoft::WRL::ComPtr<ID3D11Device> create_backend_test_device() {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr,
                                      D3D_DRIVER_TYPE_WARP,
                                      nullptr,
                                      0,
                                      levels,
                                      1,
                                      D3D11_SDK_VERSION,
                                      &device,
                                      nullptr,
                                      &context)));
  return device;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> create_backend_test_target(
    ID3D11Device* device) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = 16;
  desc.Height = 8;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
  return texture;
}

bool near_byte(uint8_t actual, int expected) {
  return std::abs(static_cast<int>(actual) - expected) <= 1;
}

}  // namespace

TEST_CASE("Windows D3D11 backend publishes a complete runner-owned target",
          "[windows_d3d11_backend]") {
  auto device = create_backend_test_device();
  std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 3> textures = {
      create_backend_test_target(device.Get()),
      create_backend_test_target(device.Get()),
      create_backend_test_target(device.Get()),
  };
  const void* raw[] = {
      textures[0].Get(), textures[1].Get(), textures[2].Get()};
  WindowsD3D11TargetRingInstall install;
  install.textures = raw;
  install.texture_count = std::size(raw);
  install.width = 16;
  install.height = 8;
  install.format = DXGI_FORMAT_B8G8R8A8_UNORM;

  auto backend = create_presentation_backend(RenderBackendKind::NativeD3D11);
  REQUIRE(backend != nullptr);
  PresentationBackendConfig config;
  config.width = 16;
  config.height = 8;
  config.max_track_slots = 1;
  config.offscreen = true;
  config.output = &install;
  config.output_target = ColorOutputTarget::kSDRToneMappedBT709;
  const bool initialized = backend->initialize(config);
  INFO(backend->last_error());
  REQUIRE(initialized);
  REQUIRE(backend->native_render_device() == device.Get());
  REQUIRE_FALSE(backend->update_sdr_white_level(0.0));
  REQUIRE(backend->update_sdr_white_level(100.0));

  RendererDrawSnapshot snapshot;
  snapshot.target_width = 16;
  snapshot.target_height = 8;
  snapshot.background_color[0] = 0.25f;
  snapshot.background_color[1] = 0.50f;
  snapshot.background_color[2] = 0.75f;
  snapshot.background_color[3] = 1.00f;
  REQUIRE(backend->draw_frame(snapshot, {}));

  PresentationBackendFrameInfo frame_info;
  REQUIRE(backend->copy_last_frame_info(&frame_info));
  REQUIRE(frame_info.target_pixel_buffer_address != 0);
  auto* completed = reinterpret_cast<ID3D11Texture2D*>(
      frame_info.target_pixel_buffer_address);
  backend->mark_offscreen_target_displayed(completed);

  std::vector<uint8_t> bgra;
  int width = 0;
  int height = 0;
  REQUIRE(backend->capture_front_buffer(bgra, width, height));
  REQUIRE(width == 16);
  REQUIRE(height == 8);
  REQUIRE(bgra.size() == 16u * 8u * 4u);
  REQUIRE(near_byte(bgra[0], 191));
  REQUIRE(near_byte(bgra[1], 128));
  REQUIRE(near_byte(bgra[2], 64));
  REQUIRE(near_byte(bgra[3], 255));

  const auto stats = backend->presentation_stats();
  REQUIRE(stats.backend_available == 1);
  REQUIRE(stats.target_installed == 1);
  REQUIRE(stats.last_draw_succeeded == 1);
  REQUIRE(stats.viewport_composite_count == 1);
  const auto diagnostics = backend->diagnostics();
  REQUIRE(diagnostics.backend == "windows-native-d3d11");
  REQUIRE(diagnostics.target_format == "bgra8");
  REQUIRE(diagnostics.fallback_reason == "none");
  REQUIRE(diagnostics.buffer_count == 3);
  backend->shutdown();
}
