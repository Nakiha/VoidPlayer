#include "video_renderer/decode/software_bgra_converter.h"

#include <algorithm>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace vr {
namespace {

uint8_t clamp_u8(int value) {
  return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

void yuv_to_bgra(uint8_t y,
                 uint8_t u,
                 uint8_t v,
                 bool full_range,
                 uint8_t* out) {
  const int uu = static_cast<int>(u) - 128;
  const int vv = static_cast<int>(v) - 128;
  int r = 0;
  int g = 0;
  int b = 0;
  if (full_range) {
    const int yy = static_cast<int>(y);
    r = (256 * yy + 359 * vv + 128) >> 8;
    g = (256 * yy - 88 * uu - 183 * vv + 128) >> 8;
    b = (256 * yy + 454 * uu + 128) >> 8;
  } else {
    const int yy = std::max(0, static_cast<int>(y) - 16);
    r = (298 * yy + 409 * vv + 128) >> 8;
    g = (298 * yy - 100 * uu - 208 * vv + 128) >> 8;
    b = (298 * yy + 516 * uu + 128) >> 8;
  }
  out[0] = clamp_u8(b);
  out[1] = clamp_u8(g);
  out[2] = clamp_u8(r);
  out[3] = 255;
}

bool convert_yuv420p_to_bgra(const AVFrame* frame, uint8_t* dst) {
  const bool full_range = frame->color_range == AVCOL_RANGE_JPEG ||
                          frame->format == AV_PIX_FMT_YUVJ420P;
  const int width = frame->width;
  const int height = frame->height;
  const uint8_t* y_plane = frame->data[0];
  const uint8_t* u_plane = frame->data[1];
  const uint8_t* v_plane = frame->data[2];
  if (!y_plane || !u_plane || !v_plane) {
    return false;
  }

  for (int y = 0; y < height; ++y) {
    const uint8_t* y_row = y_plane + y * frame->linesize[0];
    const uint8_t* u_row = u_plane + (y / 2) * frame->linesize[1];
    const uint8_t* v_row = v_plane + (y / 2) * frame->linesize[2];
    uint8_t* dst_row = dst + static_cast<size_t>(y) * width * 4;
    for (int x = 0; x < width; ++x) {
      yuv_to_bgra(y_row[x], u_row[x / 2], v_row[x / 2], full_range, dst_row + x * 4);
    }
  }
  return true;
}

bool convert_nv12_to_bgra(const AVFrame* frame, uint8_t* dst) {
  const bool full_range = frame->color_range == AVCOL_RANGE_JPEG;
  const int width = frame->width;
  const int height = frame->height;
  const uint8_t* y_plane = frame->data[0];
  const uint8_t* uv_plane = frame->data[1];
  if (!y_plane || !uv_plane) {
    return false;
  }

  for (int y = 0; y < height; ++y) {
    const uint8_t* y_row = y_plane + y * frame->linesize[0];
    const uint8_t* uv_row = uv_plane + (y / 2) * frame->linesize[1];
    uint8_t* dst_row = dst + static_cast<size_t>(y) * width * 4;
    for (int x = 0; x < width; ++x) {
      const int uv_index = (x / 2) * 2;
      yuv_to_bgra(y_row[x], uv_row[uv_index], uv_row[uv_index + 1], full_range, dst_row + x * 4);
    }
  }
  return true;
}

}  // namespace

bool convert_software_frame_to_bgra(const AVFrame* frame, uint8_t* dst, size_t dst_size) {
  if (!frame || !dst || frame->width <= 0 || frame->height <= 0) {
    return false;
  }
  const size_t required_size = static_cast<size_t>(frame->width) * frame->height * 4;
  if (dst_size < required_size) {
    return false;
  }

  switch (static_cast<AVPixelFormat>(frame->format)) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
      return convert_yuv420p_to_bgra(frame, dst);
    case AV_PIX_FMT_NV12:
      return convert_nv12_to_bgra(frame, dst);
    default:
      return false;
  }
}

}  // namespace vr
