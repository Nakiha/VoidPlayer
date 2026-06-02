#include "macos/presentation_adapter.h"

#include "renderer/buffer/bidi_ring_buffer.h"
#include "renderer/decode/yuv_to_bgra.h"
#include "renderer/frame/frame_storage.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace vp_macos {
namespace {

uint8_t clamp_u8(int value) {
  return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

uint16_t read_le16(const uint8_t* data) {
  uint16_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

uint8_t p010_sample_to_u8(uint16_t p010_sample) {
  const int sample_10_bit = static_cast<int>(p010_sample >> 6);
  return clamp_u8((sample_10_bit + 2) >> 2);
}

PresentationAdapterStatus allocate_bgra(const vr::TextureFrame& frame,
                                        OwnedBGRAFrame* out) {
  if (!out || frame.width <= 0 || frame.height <= 0) {
    return PresentationAdapterStatus::InvalidDestination;
  }
  const size_t size = static_cast<size_t>(frame.width) *
      static_cast<size_t>(frame.height) * 4u;
  auto* bgra = new (std::nothrow) uint8_t[size];
  if (!bgra) {
    return PresentationAdapterStatus::AllocationFailed;
  }
  out->width = frame.width;
  out->height = frame.height;
  out->pts_us = frame.pts_us;
  out->dts_us = frame.dts_us;
  out->duration_us = frame.duration_us;
  out->bgra = bgra;
  out->bgra_size = size;
  return PresentationAdapterStatus::Ok;
}

PresentationAdapterStatus validate_bgra_destination(const vr::TextureFrame& frame,
                                                    uint8_t* dst,
                                                    size_t dst_size,
                                                    int32_t width,
                                                    int32_t height,
                                                    int32_t stride_bytes) {
  if (!dst || frame.width <= 0 || frame.height <= 0 ||
      width != frame.width || height != frame.height ||
      stride_bytes < frame.width * 4) {
    return PresentationAdapterStatus::InvalidDestination;
  }
  const size_t last_row_offset =
      static_cast<size_t>(frame.height - 1) * static_cast<size_t>(stride_bytes);
  const size_t needed = last_row_offset + static_cast<size_t>(frame.width) * 4u;
  return needed <= dst_size
      ? PresentationAdapterStatus::Ok
      : PresentationAdapterStatus::InvalidDestination;
}

bool validate_cpu_nv12_storage(const vr::TextureFrame& frame,
                               const vr::CpuNv12FrameStorage& storage,
                               int bytes_per_sample) {
  if (!storage.data || storage.y_stride <= 0 || storage.uv_stride <= 0 ||
      storage.coded_width < frame.width || storage.coded_height < frame.height ||
      storage.coded_width <= 0 || storage.coded_height <= 0) {
    return false;
  }
  if (bytes_per_sample != 1 && bytes_per_sample != 2) {
    return false;
  }
  const int coded_chroma_height = (storage.coded_height + 1) / 2;
  const int min_stride = storage.coded_width * bytes_per_sample;
  if (storage.y_stride < min_stride || storage.uv_stride < min_stride) {
    return false;
  }
  const auto y_stride = static_cast<size_t>(storage.y_stride);
  const auto uv_stride = static_cast<size_t>(storage.uv_stride);
  const auto coded_height = static_cast<size_t>(storage.coded_height);
  const auto chroma_height = static_cast<size_t>(coded_chroma_height);
  if (coded_height > std::numeric_limits<size_t>::max() / y_stride ||
      chroma_height > std::numeric_limits<size_t>::max() / uv_stride) {
    return false;
  }
  const size_t y_bytes = y_stride * coded_height;
  const size_t uv_bytes = uv_stride * chroma_height;
  return y_bytes <= std::numeric_limits<size_t>::max() - uv_bytes &&
      y_bytes + uv_bytes <= storage.data->size();
}

int frame_color_range(const vr::TextureFrame& frame) {
  return frame.color.range == vr::VIDEO_COLOR_RANGE_FULL
      ? vr::VIDEO_COLOR_RANGE_FULL
      : vr::VIDEO_COLOR_RANGE_LIMITED;
}

int frame_color_matrix(const vr::TextureFrame& frame) {
  return frame.color.matrix == vr::VIDEO_COLOR_MATRIX_UNKNOWN
      ? vr::default_yuv_color_matrix_for_size(frame.width, frame.height)
      : frame.color.matrix;
}

void write_frame_info(const vr::TextureFrame& frame, VPMacOSNativeFrameInfo* out) {
  if (!out) {
    return;
  }
  VPMacOSNativeFrameInfoInit(out);
  out->width = frame.width;
  out->height = frame.height;
  out->pts_us = frame.pts_us;
  out->dts_us = frame.dts_us;
  out->duration_us = frame.duration_us;
  out->analysis_frame_index = frame.analysis_frame_index;
  out->frame_identity_mode = static_cast<int32_t>(frame.frame_identity_mode);
  out->source_packet_index = frame.source_packet_index;
  out->source_packet_size = frame.source_packet_size;
  out->source_packet_pos = frame.source_packet_pos;
  out->source_packet_pts = frame.source_packet_pts;
  out->source_packet_dts = frame.source_packet_dts;
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
  const int color_range = frame_color_range(frame);
  const int color_matrix = frame_color_matrix(frame);
  for (int y = 0; y < frame.height; ++y) {
    const uint8_t* y_row = storage->planes[0] +
        static_cast<size_t>(y) * storage->strides[0];
    const uint8_t* u_row = storage->planes[1] +
        static_cast<size_t>(y / 2) * storage->strides[1];
    const uint8_t* v_row = storage->planes[2] +
        static_cast<size_t>(y / 2) * storage->strides[2];
    uint8_t* dst_row = dst + static_cast<size_t>(y) * stride_bytes;
    for (int x = 0; x < frame.width; ++x) {
      vr::write_yuv_to_bgra(
          y_row[x], u_row[x / 2], v_row[x / 2],
          color_range, color_matrix, dst_row + x * 4);
    }
  }
  return true;
}

bool copy_cpu_nv12_to_bgra(const vr::TextureFrame& frame,
                           uint8_t* dst,
                           int32_t stride_bytes) {
  const auto* storage = frame.cpu_nv12_storage();
  if (!storage || storage->is_p010 ||
      !validate_cpu_nv12_storage(frame, *storage, 1)) {
    return false;
  }
  const int color_range = frame_color_range(frame);
  const int color_matrix = frame_color_matrix(frame);
  const uint8_t* y_plane = storage->data->data();
  const uint8_t* uv_plane = y_plane +
      static_cast<size_t>(storage->y_stride) * storage->coded_height;
  for (int y = 0; y < frame.height; ++y) {
    const uint8_t* y_row = y_plane + static_cast<size_t>(y) * storage->y_stride;
    const uint8_t* uv_row = uv_plane + static_cast<size_t>(y / 2) * storage->uv_stride;
    uint8_t* dst_row = dst + static_cast<size_t>(y) * stride_bytes;
    for (int x = 0; x < frame.width; ++x) {
      const int uv_index = (x / 2) * 2;
      vr::write_yuv_to_bgra(
          y_row[x], uv_row[uv_index], uv_row[uv_index + 1],
          color_range, color_matrix, dst_row + x * 4);
    }
  }
  return true;
}

bool copy_cpu_p010_to_bgra(const vr::TextureFrame& frame,
                           uint8_t* dst,
                           int32_t stride_bytes) {
  const auto* storage = frame.cpu_nv12_storage();
  if (!storage || !storage->is_p010 ||
      !validate_cpu_nv12_storage(frame, *storage, 2)) {
    return false;
  }
  const int color_range = frame_color_range(frame);
  const int color_matrix = frame_color_matrix(frame);
  const uint8_t* y_plane = storage->data->data();
  const uint8_t* uv_plane = y_plane +
      static_cast<size_t>(storage->y_stride) * storage->coded_height;
  for (int y = 0; y < frame.height; ++y) {
    const uint8_t* y_row = y_plane + static_cast<size_t>(y) * storage->y_stride;
    const uint8_t* uv_row = uv_plane + static_cast<size_t>(y / 2) * storage->uv_stride;
    uint8_t* dst_row = dst + static_cast<size_t>(y) * stride_bytes;
    for (int x = 0; x < frame.width; ++x) {
      const int uv_index = (x / 2) * 4;
      vr::write_yuv_to_bgra(
          p010_sample_to_u8(read_le16(y_row + x * 2)),
          p010_sample_to_u8(read_le16(uv_row + uv_index)),
          p010_sample_to_u8(read_le16(uv_row + uv_index + 2)),
          color_range,
          color_matrix,
          dst_row + x * 4);
    }
  }
  return true;
}

}  // namespace

const char* presentation_adapter_name() {
  return "cvpixelbuffer-bgra-copy";
}

const char* presentation_adapter_status_message(PresentationAdapterStatus status) {
  switch (status) {
  case PresentationAdapterStatus::Ok:
    return "";
  case PresentationAdapterStatus::InvalidDestination:
    return "invalid BGRA destination dimensions, stride, or buffer size";
  case PresentationAdapterStatus::UnsupportedStorage:
    return "unsupported frame storage for software CVPixelBuffer adapter";
  case PresentationAdapterStatus::InvalidStorage:
    return "invalid or undersized frame storage for software CVPixelBuffer adapter";
  case PresentationAdapterStatus::AllocationFailed:
    return "failed to allocate owned BGRA frame";
  }
  return "unknown presentation adapter failure";
}

bool presentation_adapter_supports_storage(vr::FrameStorageKind kind) {
  return vr::frame_storage_has_cpu_pixels(kind);
}

PresentationAdapterStatus copy_texture_frame_to_bgra_destination_checked(
    const vr::TextureFrame& frame,
    uint8_t* dst,
    size_t dst_size,
    int32_t width,
    int32_t height,
    int32_t stride_bytes,
    VPMacOSNativeFrameInfo* out) {
  const auto destination_status =
      validate_bgra_destination(frame, dst, dst_size, width, height, stride_bytes);
  if (destination_status != PresentationAdapterStatus::Ok) {
    return destination_status;
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
    copied = frame.cpu_nv12_storage() && frame.cpu_nv12_storage()->is_p010
        ? copy_cpu_p010_to_bgra(frame, dst, stride_bytes)
        : copy_cpu_nv12_to_bgra(frame, dst, stride_bytes);
    break;
  default:
    return PresentationAdapterStatus::UnsupportedStorage;
  }
  if (!copied) {
    return PresentationAdapterStatus::InvalidStorage;
  }
  if (copied) {
    write_frame_info(frame, out);
  }
  return PresentationAdapterStatus::Ok;
}

bool copy_texture_frame_to_bgra_destination(const vr::TextureFrame& frame,
                                            uint8_t* dst,
                                            size_t dst_size,
                                            int32_t width,
                                            int32_t height,
                                            int32_t stride_bytes,
                                            VPMacOSNativeFrameInfo* out) {
  return copy_texture_frame_to_bgra_destination_checked(
      frame, dst, dst_size, width, height, stride_bytes, out) ==
      PresentationAdapterStatus::Ok;
}

bool copy_texture_frame_to_owned_bgra(const vr::TextureFrame& frame,
                                      OwnedBGRAFrame* out) {
  if (!out) {
    return false;
  }
  *out = {};
  if (allocate_bgra(frame, out) != PresentationAdapterStatus::Ok) {
    return false;
  }
  VPMacOSNativeFrameInfo info{};
  VPMacOSNativeFrameInfoInit(&info);
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

void free_owned_bgra_frame(OwnedBGRAFrame* frame) {
  if (!frame) {
    return;
  }
  delete[] frame->bgra;
  *frame = {};
}

}  // namespace vp_macos
