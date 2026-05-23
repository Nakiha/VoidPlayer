#include "macos/presentation_adapter.h"

#include "video_renderer/buffer/bidi_ring_buffer.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

bool expect_pixel(const std::vector<uint8_t>& bgra,
                  int stride,
                  int x,
                  int y,
                  uint8_t b,
                  uint8_t g,
                  uint8_t r,
                  uint8_t a) {
  const size_t offset = static_cast<size_t>(y) * static_cast<size_t>(stride) +
      static_cast<size_t>(x) * 4u;
  return offset + 3 < bgra.size() &&
      bgra[offset] == b &&
      bgra[offset + 1] == g &&
      bgra[offset + 2] == r &&
      bgra[offset + 3] == a;
}

void write_p010_sample(std::vector<uint8_t>& data, size_t byte_offset, uint8_t value_8_bit) {
  const uint16_t p010 = static_cast<uint16_t>(value_8_bit) << 8;
  data[byte_offset] = static_cast<uint8_t>(p010 & 0xffu);
  data[byte_offset + 1] = static_cast<uint8_t>((p010 >> 8) & 0xffu);
}

int check_adapter_identity() {
  const char* name = vp_macos::presentation_adapter_name();
  if (!name || std::strcmp(name, "cvpixelbuffer-bgra-copy") != 0) {
    return fail("unexpected presentation adapter name");
  }
  return 0;
}

int check_cpu_rgba_stride_copy() {
  auto source = std::make_shared<std::vector<uint8_t>>(
      std::initializer_list<uint8_t>{
          1, 2, 3, 255, 4, 5, 6, 255, 99, 99, 99, 99,
          7, 8, 9, 255, 10, 11, 12, 255, 88, 88, 88, 88,
      });
  vr::TextureFrame frame;
  frame.width = 2;
  frame.height = 2;
  frame.pts_us = 123;
  frame.dts_us = 100;
  frame.duration_us = 33;
  frame.storage = vr::CpuRgbaFrameStorage{source, 12};

  const int dst_stride = 12;
  std::vector<uint8_t> bgra(static_cast<size_t>(dst_stride) * frame.height, 0xaa);
  VPMacOSNativeFrameInfo info{};
  if (!vp_macos::copy_texture_frame_to_bgra_destination(
          frame,
          bgra.data(),
          bgra.size(),
          frame.width,
          frame.height,
          dst_stride,
          &info)) {
    return fail("failed to copy cpu rgba frame");
  }
  if (info.pts_us != 123 || info.dts_us != 100 || info.duration_us != 33 ||
      !expect_pixel(bgra, dst_stride, 0, 0, 1, 2, 3, 255) ||
      !expect_pixel(bgra, dst_stride, 1, 0, 4, 5, 6, 255) ||
      !expect_pixel(bgra, dst_stride, 0, 1, 7, 8, 9, 255) ||
      !expect_pixel(bgra, dst_stride, 1, 1, 10, 11, 12, 255) ||
      bgra[8] != 0xaa || bgra[9] != 0xaa || bgra[20] != 0xaa || bgra[21] != 0xaa) {
    return fail("unexpected cpu rgba adapter copy");
  }

  VPMacOSNativeFrame owned{};
  if (!vp_macos::copy_texture_frame_to_owned_bgra(frame, &owned) ||
      owned.bgra_size != 16 || owned.width != 2 || owned.height != 2) {
    vp_macos::free_owned_bgra_frame(&owned);
    return fail("failed to copy cpu rgba frame into owned BGRA");
  }
  vp_macos::free_owned_bgra_frame(&owned);
  return 0;
}

int check_cpu_nv12_limited_colors() {
  auto data = std::make_shared<std::vector<uint8_t>>(
      std::initializer_list<uint8_t>{
          81, 81, 145, 145,
          81, 81, 145, 145,
          90, 240, 54, 34,
      });
  vr::TextureFrame frame;
  frame.width = 4;
  frame.height = 2;
  frame.is_nv12 = true;
  frame.color.range = vr::VIDEO_COLOR_RANGE_LIMITED;
  frame.storage = vr::CpuNv12FrameStorage{
      data,
      4,
      4,
      false,
      4,
      2,
  };

  const int dst_stride = 20;
  std::vector<uint8_t> bgra(static_cast<size_t>(dst_stride) * frame.height, 0);
  VPMacOSNativeFrameInfo info{};
  if (!vp_macos::copy_texture_frame_to_bgra_destination(
          frame,
          bgra.data(),
          bgra.size(),
          frame.width,
          frame.height,
          dst_stride,
          &info)) {
    return fail("failed to copy cpu nv12 frame");
  }
  if (!expect_pixel(bgra, dst_stride, 0, 0, 0, 0, 255, 255) ||
      !expect_pixel(bgra, dst_stride, 1, 0, 0, 0, 255, 255) ||
      !expect_pixel(bgra, dst_stride, 2, 0, 1, 255, 0, 255) ||
      !expect_pixel(bgra, dst_stride, 3, 0, 1, 255, 0, 255)) {
    return fail("unexpected cpu nv12 adapter pixels");
  }

  frame.is_p010 = true;
  frame.storage = vr::CpuNv12FrameStorage{data, 4, 4, true, 4, 2};
  if (vp_macos::copy_texture_frame_to_bgra_destination(
          frame,
          bgra.data(),
          bgra.size(),
          frame.width,
          frame.height,
          dst_stride,
          &info)) {
    return fail("adapter accepted undersized P010 software frame");
  }
  return 0;
}

int check_cpu_nv12_bt709_limited_red() {
  auto data = std::make_shared<std::vector<uint8_t>>(
      std::initializer_list<uint8_t>{
          63, 63,
          63, 63,
          102, 240,
      });
  vr::TextureFrame frame;
  frame.width = 2;
  frame.height = 2;
  frame.is_nv12 = true;
  frame.color.range = vr::VIDEO_COLOR_RANGE_LIMITED;
  frame.color.matrix = vr::VIDEO_COLOR_MATRIX_BT709;
  frame.storage = vr::CpuNv12FrameStorage{
      data,
      2,
      2,
      false,
      2,
      2,
  };

  std::vector<uint8_t> bgra(16, 0);
  VPMacOSNativeFrameInfo info{};
  if (!vp_macos::copy_texture_frame_to_bgra_destination(
          frame,
          bgra.data(),
          bgra.size(),
          frame.width,
          frame.height,
          frame.width * 4,
          &info)) {
    return fail("failed to copy bt709 nv12 frame");
  }
  for (int i = 0; i < 4; ++i) {
    const size_t offset = static_cast<size_t>(i) * 4u;
    if (bgra[offset] != 0 || bgra[offset + 1] != 1 ||
        bgra[offset + 2] != 255 || bgra[offset + 3] != 255) {
      return fail("unexpected bt709 nv12 adapter pixel");
    }
  }
  return 0;
}

int check_cpu_p010_limited_colors() {
  auto data = std::make_shared<std::vector<uint8_t>>(24, 0);
  const int y_stride = 8;
  const int uv_stride = 8;
  const std::array<uint8_t, 8> y_values = {
      81, 81, 145, 145,
      81, 81, 145, 145,
  };
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 4; ++x) {
      write_p010_sample(
          *data,
          static_cast<size_t>(y) * y_stride + static_cast<size_t>(x) * 2u,
          y_values[static_cast<size_t>(y) * 4u + static_cast<size_t>(x)]);
    }
  }
  const size_t uv_offset = static_cast<size_t>(y_stride) * 2u;
  const std::array<uint8_t, 4> uv_values = {90, 240, 54, 34};
  for (int i = 0; i < 4; ++i) {
    write_p010_sample(*data, uv_offset + static_cast<size_t>(i) * 2u, uv_values[i]);
  }

  vr::TextureFrame frame;
  frame.width = 4;
  frame.height = 2;
  frame.is_nv12 = true;
  frame.is_p010 = true;
  frame.color.range = vr::VIDEO_COLOR_RANGE_LIMITED;
  frame.storage = vr::CpuNv12FrameStorage{
      data,
      y_stride,
      uv_stride,
      true,
      4,
      2,
  };

  const int dst_stride = 20;
  std::vector<uint8_t> bgra(static_cast<size_t>(dst_stride) * frame.height, 0);
  VPMacOSNativeFrameInfo info{};
  if (!vp_macos::copy_texture_frame_to_bgra_destination(
          frame,
          bgra.data(),
          bgra.size(),
          frame.width,
          frame.height,
          dst_stride,
          &info)) {
    return fail("failed to copy cpu p010 frame");
  }
  if (!expect_pixel(bgra, dst_stride, 0, 0, 0, 0, 255, 255) ||
      !expect_pixel(bgra, dst_stride, 1, 0, 0, 0, 255, 255) ||
      !expect_pixel(bgra, dst_stride, 2, 0, 1, 255, 0, 255) ||
      !expect_pixel(bgra, dst_stride, 3, 0, 1, 255, 0, 255)) {
    return fail("unexpected cpu p010 adapter pixels");
  }
  return 0;
}

int check_cpu_planar_full_range_red() {
  std::array<uint8_t, 4> y = {76, 76, 76, 76};
  std::array<uint8_t, 1> u = {85};
  std::array<uint8_t, 1> v = {255};
  vr::TextureFrame frame;
  frame.width = 2;
  frame.height = 2;
  frame.color.range = vr::VIDEO_COLOR_RANGE_FULL;
  frame.storage = vr::CpuPlanarYuvFrameStorage{
      std::shared_ptr<void>(&y, [](void*) {}),
      {y.data(), u.data(), v.data()},
      {2, 1, 1},
      {2, 1, 1},
      {2, 1, 1},
      1,
  };

  std::vector<uint8_t> bgra(16, 0);
  VPMacOSNativeFrameInfo info{};
  if (!vp_macos::copy_texture_frame_to_bgra_destination(
          frame,
          bgra.data(),
          bgra.size(),
          frame.width,
          frame.height,
          frame.width * 4,
          &info)) {
    return fail("failed to copy cpu planar frame");
  }
  for (int i = 0; i < 4; ++i) {
    const size_t offset = static_cast<size_t>(i) * 4u;
    if (bgra[offset] != 0 || bgra[offset + 1] != 0 ||
        bgra[offset + 2] != 254 || bgra[offset + 3] != 255) {
      return fail("unexpected cpu planar full-range red pixel");
    }
  }
  return 0;
}

}  // namespace

int main() {
  if (const int ret = check_adapter_identity(); ret != 0) {
    return ret;
  }
  if (const int ret = check_cpu_rgba_stride_copy(); ret != 0) {
    return ret;
  }
  if (const int ret = check_cpu_nv12_limited_colors(); ret != 0) {
    return ret;
  }
  if (const int ret = check_cpu_nv12_bt709_limited_red(); ret != 0) {
    return ret;
  }
  if (const int ret = check_cpu_p010_limited_colors(); ret != 0) {
    return ret;
  }
  if (const int ret = check_cpu_planar_full_range_red(); ret != 0) {
    return ret;
  }
  return 0;
}
