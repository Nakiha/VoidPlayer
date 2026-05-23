#include "video_renderer/decode/software_bgra_converter.h"

#include <array>
#include <cstdint>
#include <cstdio>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

template <size_t N>
bool expect_pixel(const std::array<uint8_t, N>& bgra,
                  size_t pixel,
                  uint8_t b,
                  uint8_t g,
                  uint8_t r,
                  uint8_t a) {
  const size_t offset = pixel * 4;
  if (offset + 3 >= bgra.size()) {
    return false;
  }
  return bgra[offset] == b && bgra[offset + 1] == g && bgra[offset + 2] == r &&
         bgra[offset + 3] == a;
}

int check_yuv420p_limited_black() {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    return fail("failed to allocate yuv420p frame");
  }

  std::array<uint8_t, 4> y = {16, 16, 16, 16};
  std::array<uint8_t, 1> u = {128};
  std::array<uint8_t, 1> v = {128};
  std::array<uint8_t, 16> bgra = {};

  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = 2;
  frame->height = 2;
  frame->color_range = AVCOL_RANGE_MPEG;
  frame->data[0] = y.data();
  frame->data[1] = u.data();
  frame->data[2] = v.data();
  frame->linesize[0] = 2;
  frame->linesize[1] = 1;
  frame->linesize[2] = 1;

  const bool converted = vr::convert_software_frame_to_bgra(frame, bgra.data(), bgra.size());
  av_frame_free(&frame);
  if (!converted) {
    return fail("failed to convert yuv420p limited black");
  }
  for (size_t i = 0; i < 4; ++i) {
    if (!expect_pixel(bgra, i, 0, 0, 0, 255)) {
      return fail("unexpected yuv420p limited black pixel");
    }
  }
  return 0;
}

int check_nv12_limited_white() {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    return fail("failed to allocate nv12 frame");
  }

  std::array<uint8_t, 4> y = {235, 235, 235, 235};
  std::array<uint8_t, 2> uv = {128, 128};
  std::array<uint8_t, 16> bgra = {};

  frame->format = AV_PIX_FMT_NV12;
  frame->width = 2;
  frame->height = 2;
  frame->color_range = AVCOL_RANGE_MPEG;
  frame->data[0] = y.data();
  frame->data[1] = uv.data();
  frame->linesize[0] = 2;
  frame->linesize[1] = 2;

  const bool converted = vr::convert_software_frame_to_bgra(frame, bgra.data(), bgra.size());
  const bool rejected_short_buffer = !vr::convert_software_frame_to_bgra(frame, bgra.data(), 15);
  av_frame_free(&frame);
  if (!converted) {
    return fail("failed to convert nv12 limited white");
  }
  if (!rejected_short_buffer) {
    return fail("converter accepted undersized BGRA buffer");
  }
  for (size_t i = 0; i < 4; ++i) {
    if (!expect_pixel(bgra, i, 255, 255, 255, 255)) {
      return fail("unexpected nv12 limited white pixel");
    }
  }
  return 0;
}

int check_yuvj420p_full_range_red_with_padding() {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    return fail("failed to allocate yuvj420p frame");
  }

  std::array<uint8_t, 8> y = {
      76, 76, 99, 99,
      76, 76, 99, 99,
  };
  std::array<uint8_t, 2> u = {85, 99};
  std::array<uint8_t, 2> v = {255, 99};
  std::array<uint8_t, 16> bgra = {};

  frame->format = AV_PIX_FMT_YUVJ420P;
  frame->width = 2;
  frame->height = 2;
  frame->color_range = AVCOL_RANGE_JPEG;
  frame->data[0] = y.data();
  frame->data[1] = u.data();
  frame->data[2] = v.data();
  frame->linesize[0] = 4;
  frame->linesize[1] = 2;
  frame->linesize[2] = 2;

  const bool converted = vr::convert_software_frame_to_bgra(frame, bgra.data(), bgra.size());
  av_frame_free(&frame);
  if (!converted) {
    return fail("failed to convert yuvj420p full-range red");
  }
  for (size_t i = 0; i < 4; ++i) {
    if (!expect_pixel(bgra, i, 0, 0, 254, 255)) {
      return fail("unexpected yuvj420p full-range red pixel");
    }
  }
  return 0;
}

int check_nv12_limited_primary_colors() {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    return fail("failed to allocate nv12 primary frame");
  }

  std::array<uint8_t, 8> y = {
      81, 81, 145, 145,
      81, 81, 145, 145,
  };
  std::array<uint8_t, 8> uv = {
      90, 240, 54, 34,
      90, 240, 54, 34,
  };
  std::array<uint8_t, 32> bgra = {};

  frame->format = AV_PIX_FMT_NV12;
  frame->width = 4;
  frame->height = 2;
  frame->color_range = AVCOL_RANGE_MPEG;
  frame->data[0] = y.data();
  frame->data[1] = uv.data();
  frame->linesize[0] = 4;
  frame->linesize[1] = 4;

  const bool converted = vr::convert_software_frame_to_bgra(frame, bgra.data(), bgra.size());
  av_frame_free(&frame);
  if (!converted) {
    return fail("failed to convert nv12 limited primaries");
  }
  if (!expect_pixel(bgra, 0, 0, 0, 255, 255) ||
      !expect_pixel(bgra, 1, 0, 0, 255, 255) ||
      !expect_pixel(bgra, 2, 1, 255, 0, 255) ||
      !expect_pixel(bgra, 3, 1, 255, 0, 255)) {
    return fail("unexpected nv12 limited primary pixel");
  }
  return 0;
}

int check_nv12_bt709_limited_red() {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    return fail("failed to allocate nv12 bt709 frame");
  }

  std::array<uint8_t, 4> y = {63, 63, 63, 63};
  std::array<uint8_t, 2> uv = {102, 240};
  std::array<uint8_t, 16> bgra = {};

  frame->format = AV_PIX_FMT_NV12;
  frame->width = 2;
  frame->height = 2;
  frame->color_range = AVCOL_RANGE_MPEG;
  frame->colorspace = AVCOL_SPC_BT709;
  frame->data[0] = y.data();
  frame->data[1] = uv.data();
  frame->linesize[0] = 2;
  frame->linesize[1] = 2;

  const bool converted = vr::convert_software_frame_to_bgra(frame, bgra.data(), bgra.size());
  av_frame_free(&frame);
  if (!converted) {
    return fail("failed to convert nv12 bt709 red");
  }
  for (size_t i = 0; i < 4; ++i) {
    if (!expect_pixel(bgra, i, 0, 1, 255, 255)) {
      return fail("unexpected nv12 bt709 red pixel");
    }
  }
  return 0;
}

}  // namespace

int main() {
  if (const int ret = check_yuv420p_limited_black(); ret != 0) {
    return ret;
  }
  if (const int ret = check_nv12_limited_white(); ret != 0) {
    return ret;
  }
  if (const int ret = check_yuvj420p_full_range_red_with_padding(); ret != 0) {
    return ret;
  }
  if (const int ret = check_nv12_limited_primary_colors(); ret != 0) {
    return ret;
  }
  if (const int ret = check_nv12_bt709_limited_red(); ret != 0) {
    return ret;
  }
  return 0;
}
