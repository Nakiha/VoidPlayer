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

bool expect_pixel(const std::array<uint8_t, 16>& bgra,
                  size_t pixel,
                  uint8_t b,
                  uint8_t g,
                  uint8_t r,
                  uint8_t a) {
  const size_t offset = pixel * 4;
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

}  // namespace

int main() {
  if (const int ret = check_yuv420p_limited_black(); ret != 0) {
    return ret;
  }
  if (const int ret = check_nv12_limited_white(); ret != 0) {
    return ret;
  }
  return 0;
}
