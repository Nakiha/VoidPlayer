#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/color/color_reference.h"
#include "renderer/render/presentation_snapshot.h"
#include "windows/presentation/windows_d3d11_viewport_renderer.h"

#include <DirectXPackedVector.h>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace vr;

namespace {

struct ViewportTestDevice {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
};

ViewportTestDevice create_viewport_test_device() {
  ViewportTestDevice result;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr,
                                      D3D_DRIVER_TYPE_WARP,
                                      nullptr,
                                      0,
                                      levels,
                                      1,
                                      D3D11_SDK_VERSION,
                                      &result.device,
                                      nullptr,
                                      &result.context)));
  return result;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> create_viewport_target(
    ID3D11Device* device,
    DXGI_FORMAT format) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = 16;
  desc.Height = 8;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
  return texture;
}

std::vector<uint8_t> read_viewport_target(ViewportTestDevice& test_device,
                                          ID3D11Texture2D* source) {
  D3D11_TEXTURE2D_DESC desc = {};
  source->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  REQUIRE(SUCCEEDED(test_device.device->CreateTexture2D(
      &desc, nullptr, &staging)));
  test_device.context->CopyResource(staging.Get(), source);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  REQUIRE(SUCCEEDED(test_device.context->Map(
      staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
  const size_t pixel_bytes = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
      ? 8u
      : 4u;
  const size_t row_bytes = static_cast<size_t>(desc.Width) * pixel_bytes;
  std::vector<uint8_t> result(row_bytes * desc.Height);
  for (UINT row = 0; row < desc.Height; ++row) {
    std::memcpy(result.data() + static_cast<size_t>(row) * row_bytes,
                static_cast<const uint8_t*>(mapped.pData) +
                    static_cast<size_t>(row) * mapped.RowPitch,
                row_bytes);
  }
  test_device.context->Unmap(staging.Get(), 0);
  return result;
}

RendererDrawSnapshot make_single_track_snapshot(TextureFrame frame) {
  RendererDrawSnapshot snapshot;
  snapshot.target_width = 16;
  snapshot.target_height = 8;
  snapshot.background_color[3] = 1.0f;
  snapshot.tracks[0].active = true;
  snapshot.tracks[0].file_id = 7;
  snapshot.tracks[0].generation = 11;
  snapshot.tracks[0].video_width = frame.width;
  snapshot.tracks[0].video_height = frame.height;
  snapshot.tracks[0].video_aspect =
      static_cast<float>(frame.width) / static_cast<float>(frame.height);
  snapshot.track_geometry[0].active = true;
  snapshot.track_geometry[0].width = frame.width;
  snapshot.track_geometry[0].height = frame.height;
  snapshot.track_geometry[0].aspect = snapshot.tracks[0].video_aspect;
  snapshot.decision.should_present = true;
  snapshot.decision.file_ids[0] = 7;
  snapshot.decision.track_generations[0] = 11;
  snapshot.decision.frames[0] = std::move(frame);
  return snapshot;
}

PresentationSnapshot build_snapshot(const RendererDrawSnapshot& draw) {
  return build_presentation_snapshot(draw.decision,
                                     draw.layout,
                                     draw.track_geometry,
                                     draw.target_width,
                                     draw.target_height,
                                     draw.background_color);
}

size_t bgra_pixel_offset(int x, int y) {
  return (static_cast<size_t>(y) * 16u + static_cast<size_t>(x)) * 4u;
}

size_t fp16_pixel_offset(int x, int y) {
  return (static_cast<size_t>(y) * 16u + static_cast<size_t>(x)) * 8u;
}

}  // namespace

TEST_CASE("Windows D3D11 viewport matches shared SDR NV12 color reference",
          "[windows_d3d11_viewport][windows_color]") {
  auto test_device = create_viewport_test_device();
  WindowsD3D11ViewportRenderer renderer;
  REQUIRE(renderer.initialize(test_device.device.Get(), test_device.context.Get()));
  auto target = create_viewport_target(test_device.device.Get(),
                                       DXGI_FORMAT_B8G8R8A8_UNORM);
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  REQUIRE(SUCCEEDED(test_device.device->CreateRenderTargetView(
      target.Get(), nullptr, &rtv)));

  auto pixels = std::make_shared<std::vector<uint8_t>>(24u, 128u);
  std::fill_n(pixels->begin(), 16, static_cast<uint8_t>(126));
  TextureFrame frame;
  frame.width = 4;
  frame.height = 4;
  frame.color = {VIDEO_COLOR_RANGE_LIMITED,
                 VIDEO_COLOR_MATRIX_BT709,
                 VIDEO_COLOR_TRANSFER_SDR,
                 VIDEO_COLOR_PRIMARIES_BT709};
  frame.storage = CpuNv12FrameStorage{pixels, 4, 4, false, 4, 4};
  auto draw = make_single_track_snapshot(std::move(frame));
  const auto presentation = build_snapshot(draw);
  REQUIRE(renderer.draw(draw,
                        presentation,
                        rtv.Get(),
                        ColorOutputTarget::kSDRToneMappedBT709,
                        80.0));

  const auto readback = read_viewport_target(test_device, target.Get());
  const auto offset = bgra_pixel_offset(8, 4);
  ColorReferenceConfig config;
  config.range = VIDEO_COLOR_RANGE_LIMITED;
  config.matrix = VIDEO_COLOR_MATRIX_BT709;
  config.transfer = VIDEO_COLOR_TRANSFER_SDR;
  config.primaries = VIDEO_COLOR_PRIMARIES_BT709;
  const auto expected = color_reference_sample_yuv(
      {126.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0}, config);
  REQUIRE(static_cast<int>(readback[offset + 2]) ==
          Catch::Approx(expected.r * 255.0).margin(2.0));
  REQUIRE(static_cast<int>(readback[offset + 1]) ==
          Catch::Approx(expected.g * 255.0).margin(2.0));
  REQUIRE(static_cast<int>(readback[offset + 0]) ==
          Catch::Approx(expected.b * 255.0).margin(2.0));
  REQUIRE(readback[offset + 3] == 255);
  REQUIRE(renderer.stats().software_frame_count == 1);
}

TEST_CASE("Windows D3D11 viewport writes P010 SDR into linear scRGB",
          "[windows_d3d11_viewport][windows_color][windows_fp16]") {
  auto test_device = create_viewport_test_device();
  WindowsD3D11ViewportRenderer renderer;
  REQUIRE(renderer.initialize(test_device.device.Get(), test_device.context.Get()));
  auto target = create_viewport_target(test_device.device.Get(),
                                       DXGI_FORMAT_R16G16B16A16_FLOAT);
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  REQUIRE(SUCCEEDED(test_device.device->CreateRenderTargetView(
      target.Get(), nullptr, &rtv)));

  auto pixels = std::make_shared<std::vector<uint8_t>>(48u, 0u);
  const uint16_t y_value = static_cast<uint16_t>(512u << 6u);
  const uint16_t chroma_value = static_cast<uint16_t>(512u << 6u);
  for (size_t offset = 0; offset < 32u; offset += 2u) {
    std::memcpy(pixels->data() + offset, &y_value, sizeof(y_value));
  }
  for (size_t offset = 32u; offset < pixels->size(); offset += 2u) {
    std::memcpy(pixels->data() + offset, &chroma_value, sizeof(chroma_value));
  }
  TextureFrame frame;
  frame.width = 4;
  frame.height = 4;
  frame.color = {VIDEO_COLOR_RANGE_LIMITED,
                 VIDEO_COLOR_MATRIX_BT709,
                 VIDEO_COLOR_TRANSFER_SDR,
                 VIDEO_COLOR_PRIMARIES_BT709};
  frame.storage = CpuNv12FrameStorage{pixels, 8, 8, true, 4, 4};
  auto draw = make_single_track_snapshot(std::move(frame));
  const auto presentation = build_snapshot(draw);
  REQUIRE(renderer.draw(draw,
                        presentation,
                        rtv.Get(),
                        ColorOutputTarget::kWindowsLinearScRGB,
                        100.0));

  const auto readback = read_viewport_target(test_device, target.Get());
  const auto offset = fp16_pixel_offset(8, 4);
  std::array<uint16_t, 4> half{};
  std::memcpy(half.data(), readback.data() + offset, 8u);
  const float red = DirectX::PackedVector::XMConvertHalfToFloat(half[0]);
  const float green = DirectX::PackedVector::XMConvertHalfToFloat(half[1]);
  const float blue = DirectX::PackedVector::XMConvertHalfToFloat(half[2]);
  const float alpha = DirectX::PackedVector::XMConvertHalfToFloat(half[3]);
  ColorReferenceConfig config;
  config.range = VIDEO_COLOR_RANGE_LIMITED;
  config.matrix = VIDEO_COLOR_MATRIX_BT709;
  config.transfer = VIDEO_COLOR_TRANSFER_SDR;
  config.primaries = VIDEO_COLOR_PRIMARIES_BT709;
  config.output_target = ColorOutputTarget::kWindowsLinearScRGB;
  config.sdr_white_level_nits = 100.0;
  const auto expected = color_reference_sample_yuv(
      {512.0 / 1023.0, 512.0 / 1023.0, 512.0 / 1023.0}, config);
  REQUIRE(red == Catch::Approx(expected.r).margin(0.004));
  REQUIRE(green == Catch::Approx(expected.g).margin(0.004));
  REQUIRE(blue == Catch::Approx(expected.b).margin(0.004));
  REQUIRE(alpha == Catch::Approx(1.0f));
}

TEST_CASE("Windows D3D11 viewport samples the selected D3D11VA array slice",
          "[windows_d3d11_viewport][windows_d3d11va][windows_color]") {
  auto test_device = create_viewport_test_device();
  WindowsD3D11ViewportRenderer renderer;
  REQUIRE(renderer.initialize(test_device.device.Get(), test_device.context.Get()));
  auto target = create_viewport_target(test_device.device.Get(),
                                       DXGI_FORMAT_B8G8R8A8_UNORM);
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  REQUIRE(SUCCEEDED(test_device.device->CreateRenderTargetView(
      target.Get(), nullptr, &rtv)));

  std::array<uint8_t, 24> dark{};
  std::array<uint8_t, 24> gray{};
  std::fill_n(dark.begin(), 16, static_cast<uint8_t>(16));
  std::fill(dark.begin() + 16, dark.end(), static_cast<uint8_t>(128));
  std::fill_n(gray.begin(), 16, static_cast<uint8_t>(126));
  std::fill(gray.begin() + 16, gray.end(), static_cast<uint8_t>(128));
  D3D11_TEXTURE2D_DESC source_desc = {};
  source_desc.Width = 4;
  source_desc.Height = 4;
  source_desc.MipLevels = 1;
  source_desc.ArraySize = 2;
  source_desc.Format = DXGI_FORMAT_NV12;
  source_desc.SampleDesc.Count = 1;
  source_desc.Usage = D3D11_USAGE_DEFAULT;
  source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
  REQUIRE(SUCCEEDED(test_device.device->CreateTexture2D(
      &source_desc, nullptr, &source)));
  test_device.context->UpdateSubresource(
      source.Get(), D3D11CalcSubresource(0, 0, 1), nullptr, dark.data(), 4, 24);
  test_device.context->UpdateSubresource(
      source.Get(), D3D11CalcSubresource(0, 1, 1), nullptr, gray.data(), 4, 24);

  TextureFrame frame;
  frame.width = 4;
  frame.height = 4;
  frame.color = {VIDEO_COLOR_RANGE_LIMITED,
                 VIDEO_COLOR_MATRIX_BT709,
                 VIDEO_COLOR_TRANSFER_SDR,
                 VIDEO_COLOR_PRIMARIES_BT709};
  auto frame_ref = std::make_shared<int>(1);
  frame.storage = WindowsD3D11FrameStorage{
      source.Get(), 1, false, 4, 4, frame_ref};
  auto draw = make_single_track_snapshot(std::move(frame));
  const auto presentation = build_snapshot(draw);
  REQUIRE(renderer.draw(draw,
                        presentation,
                        rtv.Get(),
                        ColorOutputTarget::kSDRToneMappedBT709,
                        80.0));

  const auto readback = read_viewport_target(test_device, target.Get());
  const auto offset = bgra_pixel_offset(8, 4);
  ColorReferenceConfig config;
  config.range = VIDEO_COLOR_RANGE_LIMITED;
  config.matrix = VIDEO_COLOR_MATRIX_BT709;
  config.transfer = VIDEO_COLOR_TRANSFER_SDR;
  config.primaries = VIDEO_COLOR_PRIMARIES_BT709;
  const auto expected = color_reference_sample_yuv(
      {126.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0}, config);
  REQUIRE(static_cast<int>(readback[offset + 2]) ==
          Catch::Approx(expected.r * 255.0).margin(2.0));
  REQUIRE(static_cast<int>(readback[offset + 1]) ==
          Catch::Approx(expected.g * 255.0).margin(2.0));
  REQUIRE(static_cast<int>(readback[offset + 0]) ==
          Catch::Approx(expected.b * 255.0).margin(2.0));
  REQUIRE(renderer.stats().hardware_frame_count == 1);
  REQUIRE(renderer.stats().video_source_update_count == 1);
  REQUIRE(renderer.stats().source_frame_cache_miss_count == 1);
  REQUIRE(renderer.stats().source_frame_cache_hit_count == 0);

  ++draw.layout_revision;
  const auto projected_presentation = build_snapshot(draw);
  REQUIRE(renderer.draw(draw,
                        projected_presentation,
                        rtv.Get(),
                        ColorOutputTarget::kSDRToneMappedBT709,
                        80.0));
  REQUIRE(renderer.stats().hardware_frame_count == 2);
  REQUIRE(renderer.stats().video_source_update_count == 1);
  REQUIRE(renderer.stats().source_frame_cache_miss_count == 1);
  REQUIRE(renderer.stats().source_frame_cache_hit_count == 1);
}

TEST_CASE("Windows D3D11 viewport applies shared split layout and track order",
          "[windows_d3d11_viewport][windows_layout]") {
  auto test_device = create_viewport_test_device();
  WindowsD3D11ViewportRenderer renderer;
  REQUIRE(renderer.initialize(test_device.device.Get(), test_device.context.Get()));
  auto target = create_viewport_target(test_device.device.Get(),
                                       DXGI_FORMAT_B8G8R8A8_UNORM);
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  REQUIRE(SUCCEEDED(test_device.device->CreateRenderTargetView(
      target.Get(), nullptr, &rtv)));

  auto red_pixels = std::make_shared<std::vector<uint8_t>>(16u);
  auto green_pixels = std::make_shared<std::vector<uint8_t>>(16u);
  for (size_t offset = 0; offset < 16u; offset += 4u) {
    (*red_pixels)[offset + 2] = 255;
    (*red_pixels)[offset + 3] = 255;
    (*green_pixels)[offset + 1] = 255;
    (*green_pixels)[offset + 3] = 255;
  }
  TextureFrame red;
  red.width = 2;
  red.height = 2;
  red.color = {VIDEO_COLOR_RANGE_FULL,
               VIDEO_COLOR_MATRIX_BT709,
               VIDEO_COLOR_TRANSFER_SDR,
               VIDEO_COLOR_PRIMARIES_BT709};
  red.storage = CpuRgbaFrameStorage{red_pixels, 8};
  TextureFrame green = red;
  green.storage = CpuRgbaFrameStorage{green_pixels, 8};

  auto draw = make_single_track_snapshot(std::move(red));
  draw.layout.mode = LAYOUT_SPLIT_SCREEN;
  draw.layout.split_pos = 0.5f;
  draw.layout.order[0] = 1;
  draw.layout.order[1] = 0;
  draw.tracks[1].active = true;
  draw.tracks[1].file_id = 8;
  draw.tracks[1].generation = 12;
  draw.tracks[1].video_width = 2;
  draw.tracks[1].video_height = 2;
  draw.tracks[1].video_aspect = 1.0f;
  draw.track_geometry[1] = {true, 2, 2, 1.0f};
  draw.decision.file_ids[1] = 8;
  draw.decision.track_generations[1] = 12;
  draw.decision.frames[1] = std::move(green);
  const auto presentation = build_snapshot(draw);
  REQUIRE(renderer.draw(draw,
                        presentation,
                        rtv.Get(),
                        ColorOutputTarget::kSDRToneMappedBT709,
                        80.0));

  const auto readback = read_viewport_target(test_device, target.Get());
  const auto left = bgra_pixel_offset(5, 4);
  const auto right = bgra_pixel_offset(11, 4);
  REQUIRE(readback[left + 0] == 0);
  REQUIRE(readback[left + 1] == 255);
  REQUIRE(readback[left + 2] == 0);
  REQUIRE(readback[left + 3] == 255);
  REQUIRE(readback[right + 0] == 0);
  REQUIRE(readback[right + 1] == 0);
  REQUIRE(readback[right + 2] == 255);
  REQUIRE(readback[right + 3] == 255);
}

TEST_CASE("Windows D3D11 overlay matches Metal black-white-black contrast lines",
          "[windows_d3d11_viewport][windows_overlay_layer][windows_color][windows_fp16]") {
  auto test_device = create_viewport_test_device();
  WindowsD3D11ViewportRenderer renderer;
  REQUIRE(renderer.initialize(test_device.device.Get(), test_device.context.Get()));

  PresentationSnapshot presentation;
  presentation.constants.mode = 0;
  presentation.constants.track_count = 1;
  presentation.constants.order[0] = 0;
  presentation.constants.canvas_width = 16.0f;
  presentation.constants.canvas_height = 8.0f;
  presentation.constants.inv_display_size_x[0] = 1.0f;
  presentation.constants.inv_display_size_y[0] = 1.0f;

  AnalysisOverlayPrimitivePackage package;
  AnalysisOverlayTrackPrimitives track;
  track.slot = 0;
  track.video_width = 16;
  track.video_height = 8;
  track.line_alpha = 255;
  track.outline_rects.push_back(
      {8, 0, 16, 8, analysis::OverlayColor{255, 255, 255, 255}});
  package.tracks.push_back(std::move(track));

  const D3D11_VIEWPORT viewport = {0.0f, 0.0f, 16.0f, 8.0f, 0.0f, 1.0f};
  test_device.context->RSSetViewports(1, &viewport);

  SECTION("SDR BGRA8") {
    auto target = create_viewport_target(test_device.device.Get(),
                                         DXGI_FORMAT_B8G8R8A8_UNORM);
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    REQUIRE(SUCCEEDED(test_device.device->CreateRenderTargetView(
        target.Get(), nullptr, &rtv)));
    const float gray[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    test_device.context->ClearRenderTargetView(rtv.Get(), gray);
    REQUIRE(renderer.draw_overlay(package,
                                  presentation,
                                  rtv.Get(),
                                  ColorOutputTarget::kSDRToneMappedBT709,
                                  80.0));
    const auto readback = read_viewport_target(test_device, target.Get());
    const auto channel = [&](int x) {
      return static_cast<int>(readback[bgra_pixel_offset(x, 4) + 2]);
    };
    CHECK(channel(6) == Catch::Approx(128).margin(1));
    CHECK(channel(7) == Catch::Approx(19).margin(2));
    CHECK(channel(8) == Catch::Approx(243).margin(2));
    CHECK(channel(9) == Catch::Approx(19).margin(2));
    CHECK(channel(10) == Catch::Approx(128).margin(1));
  }

  SECTION("linear scRGB FP16") {
    auto target = create_viewport_target(test_device.device.Get(),
                                         DXGI_FORMAT_R16G16B16A16_FLOAT);
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    REQUIRE(SUCCEEDED(test_device.device->CreateRenderTargetView(
        target.Get(), nullptr, &rtv)));
    const float gray[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    test_device.context->ClearRenderTargetView(rtv.Get(), gray);
    REQUIRE(renderer.draw_overlay(package,
                                  presentation,
                                  rtv.Get(),
                                  ColorOutputTarget::kWindowsLinearScRGB,
                                  100.0));
    const auto readback = read_viewport_target(test_device, target.Get());
    const auto channel = [&](int x) {
      std::array<uint16_t, 4> half{};
      std::memcpy(half.data(),
                  readback.data() + fp16_pixel_offset(x, 4),
                  sizeof(half));
      return DirectX::PackedVector::XMConvertHalfToFloat(half[0]);
    };
    CHECK(channel(6) == Catch::Approx(0.5f).margin(0.002f));
    CHECK(channel(7) == Catch::Approx(0.075f).margin(0.003f));
    CHECK(channel(8) == Catch::Approx(1.1895f).margin(0.004f));
    CHECK(channel(9) == Catch::Approx(0.075f).margin(0.003f));
    CHECK(channel(10) == Catch::Approx(0.5f).margin(0.002f));
  }
}

TEST_CASE("Windows D3D11 overlay retains source instances across layout changes",
          "[windows_d3d11_viewport][windows_overlay_layer][windows_high_refresh]") {
  auto test_device = create_viewport_test_device();
  WindowsD3D11ViewportRenderer renderer;
  REQUIRE(renderer.initialize(test_device.device.Get(), test_device.context.Get()));

  PresentationSnapshot presentation;
  presentation.constants.mode = 0;
  presentation.constants.track_count = 1;
  presentation.constants.order[0] = 0;
  presentation.constants.canvas_width = 64.0f;
  presentation.constants.canvas_height = 64.0f;
  presentation.constants.inv_display_size_x[0] = 1.0f;
  presentation.constants.inv_display_size_y[0] = 1.0f;

  AnalysisOverlayPrimitivePackage package;
  package.cache_generation = UINT64_C(0xfedcba9876543211);
  AnalysisOverlayTrackPrimitives track;
  track.slot = 0;
  track.video_width = 64;
  track.video_height = 64;
  track.outline_rects.push_back(
      {0, 0, 64, 64, analysis::OverlayColor{255, 255, 255, 255}});
  package.tracks.push_back(std::move(track));

  auto target = create_viewport_target(test_device.device.Get(),
                                       DXGI_FORMAT_B8G8R8A8_UNORM);
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  REQUIRE(SUCCEEDED(test_device.device->CreateRenderTargetView(
      target.Get(), nullptr, &rtv)));
  const D3D11_VIEWPORT viewport = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
  test_device.context->RSSetViewports(1, &viewport);

  REQUIRE(renderer.draw_overlay(package,
                                presentation,
                                rtv.Get(),
                                ColorOutputTarget::kSDRToneMappedBT709,
                                80.0));
  auto stats = renderer.stats();
  CHECK(stats.overlay_gpu_upload_count == 1);
  CHECK(stats.overlay_gpu_buffer_reuse_count == 0);
  CHECK(stats.overlay_last_source_generation == package.cache_generation);

  presentation.constants.display_offset_x[0] = 0.125f;
  presentation.constants.view_offset_uv_y[0] = 0.25f;
  REQUIRE(renderer.draw_overlay(package,
                                presentation,
                                rtv.Get(),
                                ColorOutputTarget::kSDRToneMappedBT709,
                                80.0));
  stats = renderer.stats();
  CHECK(stats.overlay_draw_count == 2);
  CHECK(stats.overlay_gpu_upload_count == 1);
  CHECK(stats.overlay_gpu_buffer_reuse_count == 1);
  CHECK(stats.overlay_source_cache_hit_count >= 1);
}

TEST_CASE("Windows D3D11 source overlay obeys parallel and split clipping",
          "[windows_d3d11_viewport][windows_overlay_layer][windows_source_projection]") {
  auto test_device = create_viewport_test_device();
  WindowsD3D11ViewportRenderer renderer;
  REQUIRE(renderer.initialize(test_device.device.Get(), test_device.context.Get()));

  PresentationSnapshot presentation;
  presentation.constants.track_count = 2;
  presentation.constants.order[0] = 0;
  presentation.constants.order[1] = 1;
  presentation.constants.canvas_width = 16.0f;
  presentation.constants.canvas_height = 8.0f;
  presentation.constants.inv_display_size_x[0] = 1.0f;
  presentation.constants.inv_display_size_y[0] = 1.0f;

  AnalysisOverlayPrimitivePackage package;
  AnalysisOverlayTrackPrimitives track;
  track.slot = 0;
  track.video_width = 16;
  track.video_height = 8;
  track.fill_rects.push_back(
      {0, 0, 16, 8, analysis::OverlayColor{255, 255, 255, 255}});
  package.tracks.push_back(std::move(track));

  const D3D11_VIEWPORT viewport = {0.0f, 0.0f, 16.0f, 8.0f, 0.0f, 1.0f};
  test_device.context->RSSetViewports(1, &viewport);
  const auto render = [&](int mode, float split) {
    presentation.constants.mode = mode;
    presentation.constants.split_pos = split;
    auto target = create_viewport_target(test_device.device.Get(),
                                         DXGI_FORMAT_B8G8R8A8_UNORM);
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    REQUIRE(SUCCEEDED(test_device.device->CreateRenderTargetView(
        target.Get(), nullptr, &rtv)));
    const float gray[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    test_device.context->ClearRenderTargetView(rtv.Get(), gray);
    REQUIRE(renderer.draw_overlay(package,
                                  presentation,
                                  rtv.Get(),
                                  ColorOutputTarget::kSDRToneMappedBT709,
                                  80.0));
    return read_viewport_target(test_device, target.Get());
  };
  const auto red_at = [](const std::vector<uint8_t>& pixels, int x) {
    return static_cast<int>(pixels[bgra_pixel_offset(x, 4) + 2]);
  };

  SECTION("parallel cell") {
    const auto pixels = render(0, 0.5f);
    CHECK(red_at(pixels, 3) == Catch::Approx(255).margin(1));
    CHECK(red_at(pixels, 7) == Catch::Approx(255).margin(1));
    CHECK(red_at(pixels, 8) == Catch::Approx(128).margin(1));
    CHECK(red_at(pixels, 12) == Catch::Approx(128).margin(1));
  }

  SECTION("split boundary") {
    const auto pixels = render(1, 0.375f);
    CHECK(red_at(pixels, 2) == Catch::Approx(255).margin(1));
    CHECK(red_at(pixels, 5) == Catch::Approx(255).margin(1));
    CHECK(red_at(pixels, 6) == Catch::Approx(128).margin(1));
    CHECK(red_at(pixels, 12) == Catch::Approx(128).margin(1));
  }
}

TEST_CASE("Windows D3D11 viewport opens an independent decode-device snapshot",
          "[windows_d3d11_viewport][windows_d3d11va][windows_cross_device]") {
  auto presentation_device = create_viewport_test_device();
  auto decode_device = create_viewport_test_device();
  WindowsD3D11ViewportRenderer renderer;
  REQUIRE(renderer.initialize(presentation_device.device.Get(),
                              presentation_device.context.Get()));
  auto target = create_viewport_target(presentation_device.device.Get(),
                                       DXGI_FORMAT_B8G8R8A8_UNORM);
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  REQUIRE(SUCCEEDED(presentation_device.device->CreateRenderTargetView(
      target.Get(), nullptr, &rtv)));

  std::array<uint8_t, 24> gray{};
  std::fill_n(gray.begin(), 16, static_cast<uint8_t>(126));
  std::fill(gray.begin() + 16, gray.end(), static_cast<uint8_t>(128));
  D3D11_TEXTURE2D_DESC source_desc = {};
  source_desc.Width = 4;
  source_desc.Height = 4;
  source_desc.MipLevels = 1;
  source_desc.ArraySize = 1;
  source_desc.Format = DXGI_FORMAT_NV12;
  source_desc.SampleDesc.Count = 1;
  source_desc.Usage = D3D11_USAGE_DEFAULT;
  source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  source_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
  REQUIRE(SUCCEEDED(decode_device.device->CreateTexture2D(
      &source_desc, nullptr, &source)));
  decode_device.context->UpdateSubresource(
      source.Get(), 0, nullptr, gray.data(), 4, 24);
  decode_device.context->Flush();

  TextureFrame frame;
  frame.width = 4;
  frame.height = 4;
  frame.color = {VIDEO_COLOR_RANGE_LIMITED,
                 VIDEO_COLOR_MATRIX_BT709,
                 VIDEO_COLOR_TRANSFER_SDR,
                 VIDEO_COLOR_PRIMARIES_BT709};
  frame.storage = WindowsD3D11FrameStorage{
      source.Get(), 0, false, 4, 4, {}};
  auto draw = make_single_track_snapshot(std::move(frame));
  const auto presentation = build_snapshot(draw);
  const bool drew = renderer.draw(draw,
                                  presentation,
                                  rtv.Get(),
                                  ColorOutputTarget::kSDRToneMappedBT709,
                                  80.0);
  INFO(renderer.last_error());
  REQUIRE(drew);

  const auto readback = read_viewport_target(presentation_device, target.Get());
  const auto offset = bgra_pixel_offset(8, 4);
  REQUIRE(static_cast<int>(readback[offset + 0]) == Catch::Approx(127).margin(2));
  REQUIRE(static_cast<int>(readback[offset + 1]) == Catch::Approx(127).margin(2));
  REQUIRE(static_cast<int>(readback[offset + 2]) == Catch::Approx(127).margin(2));
  REQUIRE(readback[offset + 3] == 255);
}
