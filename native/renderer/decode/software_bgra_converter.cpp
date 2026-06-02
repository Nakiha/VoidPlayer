#include "renderer/decode/software_bgra_converter.h"
#include "renderer/decode/yuv_to_bgra.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace vr {
namespace {

int color_range_from_frame(const AVFrame* frame) {
  return (frame->color_range == AVCOL_RANGE_JPEG ||
          frame->format == AV_PIX_FMT_YUVJ420P)
      ? VIDEO_COLOR_RANGE_FULL
      : VIDEO_COLOR_RANGE_LIMITED;
}

int color_matrix_from_frame(const AVFrame* frame) {
  switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
      return VIDEO_COLOR_MATRIX_BT709;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      return VIDEO_COLOR_MATRIX_BT2020_NCL;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
      return VIDEO_COLOR_MATRIX_BT601;
    default:
      return default_yuv_color_matrix_for_size(frame->width, frame->height);
  }
}

bool convert_yuv420p_to_bgra(const AVFrame* frame, uint8_t* dst) {
  const int color_range = color_range_from_frame(frame);
  const int color_matrix = color_matrix_from_frame(frame);
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
      write_yuv_to_bgra(
          y_row[x], u_row[x / 2], v_row[x / 2],
          color_range, color_matrix, dst_row + x * 4);
    }
  }
  return true;
}

bool convert_nv12_to_bgra(const AVFrame* frame, uint8_t* dst) {
  const int color_range = color_range_from_frame(frame);
  const int color_matrix = color_matrix_from_frame(frame);
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
      write_yuv_to_bgra(
          y_row[x], uv_row[uv_index], uv_row[uv_index + 1],
          color_range, color_matrix, dst_row + x * 4);
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
