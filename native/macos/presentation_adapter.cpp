#include "macos/presentation_adapter.h"

#include "video_renderer/buffer/bidi_ring_buffer.h"
#include "video_renderer/frame/frame_storage.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace vp_macos {
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

bool allocate_bgra(const vr::TextureFrame& frame, VPMacOSNativeFrame* out) {
  if (!out || frame.width <= 0 || frame.height <= 0) {
    return false;
  }
  const size_t size = static_cast<size_t>(frame.width) *
      static_cast<size_t>(frame.height) * 4u;
  auto* bgra = new (std::nothrow) uint8_t[size];
  if (!bgra) {
    return false;
  }
  out->width = frame.width;
  out->height = frame.height;
  out->pts_us = frame.pts_us;
  out->dts_us = frame.dts_us;
  out->duration_us = frame.duration_us;
  out->bgra = bgra;
  out->bgra_size = size;
  return true;
}

bool validate_bgra_destination(const vr::TextureFrame& frame,
                               uint8_t* dst,
                               size_t dst_size,
                               int32_t width,
                               int32_t height,
                               int32_t stride_bytes) {
  if (!dst || frame.width <= 0 || frame.height <= 0 ||
      width != frame.width || height != frame.height ||
      stride_bytes < frame.width * 4) {
    return false;
  }
  const size_t last_row_offset =
      static_cast<size_t>(frame.height - 1) * static_cast<size_t>(stride_bytes);
  const size_t needed = last_row_offset + static_cast<size_t>(frame.width) * 4u;
  return needed <= dst_size;
}

void write_frame_info(const vr::TextureFrame& frame, VPMacOSNativeFrameInfo* out) {
  if (!out) {
    return;
  }
  out->width = frame.width;
  out->height = frame.height;
  out->pts_us = frame.pts_us;
  out->dts_us = frame.dts_us;
  out->duration_us = frame.duration_us;
}

bool copy_cpu_rgba_to_bgra(const vr::TextureFrame& frame,
                           uint8_t* dst,
                           int32_t stride_bytes) {
  const auto* storage = frame.cpu_rgba_storage();
  if (!storage || !storage->data || storage->stride < frame.width * 4) {
    return false;
  }
  for (int y = 0; y < frame.height; ++y) {
    const size_t src_offset = static_cast<size_t>(y) * storage->stride;
    if (src_offset + static_cast<size_t>(frame.width) * 4u > storage->data->size()) {
      return false;
    }
    std::memcpy(dst + static_cast<size_t>(y) * stride_bytes,
                storage->data->data() + src_offset,
                static_cast<size_t>(frame.width) * 4u);
  }
  return true;
}

bool copy_cpu_planar_yuv420_to_bgra(const vr::TextureFrame& frame,
                                    uint8_t* dst,
                                    int32_t stride_bytes) {
  const auto* storage = frame.cpu_planar_yuv_storage();
  if (!storage || storage->bytes_per_sample != 1 || !storage->planes[0] ||
      !storage->planes[1] || !storage->planes[2]) {
    return false;
  }
  const bool full_range = frame.color.range == vr::VIDEO_COLOR_RANGE_FULL;
  for (int y = 0; y < frame.height; ++y) {
    const uint8_t* y_row = storage->planes[0] +
        static_cast<size_t>(y) * storage->strides[0];
    const uint8_t* u_row = storage->planes[1] +
        static_cast<size_t>(y / 2) * storage->strides[1];
    const uint8_t* v_row = storage->planes[2] +
        static_cast<size_t>(y / 2) * storage->strides[2];
    uint8_t* dst_row = dst + static_cast<size_t>(y) * stride_bytes;
    for (int x = 0; x < frame.width; ++x) {
      yuv_to_bgra(y_row[x], u_row[x / 2], v_row[x / 2],
                  full_range, dst_row + x * 4);
    }
  }
  return true;
}

bool copy_cpu_nv12_to_bgra(const vr::TextureFrame& frame,
                           uint8_t* dst,
                           int32_t stride_bytes) {
  const auto* storage = frame.cpu_nv12_storage();
  if (!storage || storage->is_p010 || !storage->data || storage->y_stride <= 0 ||
      storage->uv_stride <= 0) {
    return false;
  }
  const bool full_range = frame.color.range == vr::VIDEO_COLOR_RANGE_FULL;
  const uint8_t* y_plane = storage->data->data();
  const uint8_t* uv_plane = y_plane +
      static_cast<size_t>(storage->y_stride) * storage->coded_height;
  for (int y = 0; y < frame.height; ++y) {
    const uint8_t* y_row = y_plane + static_cast<size_t>(y) * storage->y_stride;
    const uint8_t* uv_row = uv_plane + static_cast<size_t>(y / 2) * storage->uv_stride;
    uint8_t* dst_row = dst + static_cast<size_t>(y) * stride_bytes;
    for (int x = 0; x < frame.width; ++x) {
      const int uv_index = (x / 2) * 2;
      yuv_to_bgra(y_row[x], uv_row[uv_index], uv_row[uv_index + 1],
                  full_range, dst_row + x * 4);
    }
  }
  return true;
}

}  // namespace

const char* presentation_adapter_name() {
  return "cvpixelbuffer-bgra-copy";
}

bool copy_texture_frame_to_bgra_destination(const vr::TextureFrame& frame,
                                            uint8_t* dst,
                                            size_t dst_size,
                                            int32_t width,
                                            int32_t height,
                                            int32_t stride_bytes,
                                            VPMacOSNativeFrameInfo* out) {
  if (!validate_bgra_destination(
          frame, dst, dst_size, width, height, stride_bytes)) {
    return false;
  }
  bool copied = false;
  switch (frame.storage_kind()) {
  case vr::FrameStorageKind::CpuRgba:
    copied = copy_cpu_rgba_to_bgra(frame, dst, stride_bytes);
    break;
  case vr::FrameStorageKind::CpuPlanarYuv:
    copied = copy_cpu_planar_yuv420_to_bgra(frame, dst, stride_bytes);
    break;
  case vr::FrameStorageKind::CpuNv12:
    copied = copy_cpu_nv12_to_bgra(frame, dst, stride_bytes);
    break;
  default:
    copied = false;
    break;
  }
  if (copied) {
    write_frame_info(frame, out);
  }
  return copied;
}

bool copy_texture_frame_to_owned_bgra(const vr::TextureFrame& frame,
                                      VPMacOSNativeFrame* out) {
  if (!out) {
    return false;
  }
  *out = {};
  if (!allocate_bgra(frame, out)) {
    return false;
  }
  VPMacOSNativeFrameInfo info{};
  if (!copy_texture_frame_to_bgra_destination(
          frame,
          out->bgra,
          out->bgra_size,
          frame.width,
          frame.height,
          frame.width * 4,
          &info)) {
    free_owned_bgra_frame(out);
    return false;
  }
  out->pts_us = info.pts_us;
  out->dts_us = info.dts_us;
  out->duration_us = info.duration_us;
  return true;
}

void free_owned_bgra_frame(VPMacOSNativeFrame* frame) {
  if (!frame) {
    return;
  }
  delete[] frame->bgra;
  *frame = {};
}

}  // namespace vp_macos
