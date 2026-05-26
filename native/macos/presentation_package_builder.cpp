#include "macos/presentation_package_builder.h"

#include "macos/presentation_adapter.h"
#include "video_renderer/render/presentation_snapshot.h"

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

namespace vp_macos {
namespace {

size_t align_up_size(size_t value, size_t alignment) {
  if (alignment <= 1) {
    return value;
  }
  const size_t mask = alignment - 1;
  return (value + mask) & ~mask;
}

class ScopedCVPixelBufferLock {
public:
  explicit ScopedCVPixelBufferLock(CVPixelBufferRef buffer)
      : buffer_(buffer),
        locked_(buffer &&
                CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) ==
                    kCVReturnSuccess) {}

  ~ScopedCVPixelBufferLock() {
    if (locked_) {
      CVPixelBufferUnlockBaseAddress(buffer_, kCVPixelBufferLock_ReadOnly);
    }
  }

  ScopedCVPixelBufferLock(const ScopedCVPixelBufferLock&) = delete;
  ScopedCVPixelBufferLock& operator=(const ScopedCVPixelBufferLock&) = delete;

  bool locked() const { return locked_; }

private:
  CVPixelBufferRef buffer_ = nullptr;
  bool locked_ = false;
};

}  // namespace

void fill_present_decision_info_from_snapshot(
    const vr::RendererDrawSnapshot& draw_snapshot,
    int32_t width,
    int32_t height,
    VPMacOSNativePresentDecisionInfo* out) {
  *out = {};
  const auto snapshot = vr::build_presentation_snapshot(
      draw_snapshot.decision,
      draw_snapshot.layout,
      draw_snapshot.track_geometry,
      width,
      height,
      draw_snapshot.background_color);
  const auto& constants = snapshot.constants;
  out->should_present = snapshot.should_present ? 1 : 0;
  out->current_pts_us = snapshot.current_pts_us;
  out->frame_count = snapshot.frame_count;
  out->track_count = constants.track_count;
  out->mode = constants.mode;
  out->split_pos = constants.split_pos;
  for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
    out->order[slot] = constants.order[slot];
    out->display_offset_x[slot] = constants.display_offset_x[slot];
    out->display_offset_y[slot] = constants.display_offset_y[slot];
    out->inv_display_size_x[slot] = constants.inv_display_size_x[slot];
    out->inv_display_size_y[slot] = constants.inv_display_size_y[slot];
    out->view_offset_uv_x[slot] = constants.view_offset_uv_x[slot];
    out->view_offset_uv_y[slot] = constants.view_offset_uv_y[slot];
    auto& frame_out = out->frames[slot];
    const auto& frame = snapshot.frames[slot];
    frame_out.file_id = frame.file_id;
    frame_out.slot = static_cast<int32_t>(slot);
    out->source_width[slot] = frame.width;
    out->source_height[slot] = frame.height;
    out->nv12_uv_scale_x[slot] = frame.present ? frame.nv12_uv_scale_x : 1.0f;
    out->nv12_uv_scale_y[slot] = frame.present ? frame.nv12_uv_scale_y : 1.0f;
    out->color_range[slot] = frame.color_range;
    out->color_matrix[slot] = frame.color_matrix;
    out->color_transfer[slot] = frame.color_transfer;
    out->color_primaries[slot] = frame.color_primaries;
    out->y_stride[slot] = frame.y_stride;
    out->uv_stride[slot] = frame.uv_stride;
    out->coded_width[slot] = frame.coded_width;
    out->coded_height[slot] = frame.coded_height;
    if (!frame.present) {
      continue;
    }
    frame_out.present = 1;
    frame_out.width = frame.width;
    frame_out.height = frame.height;
    frame_out.pts_us = frame.pts_us;
    frame_out.dts_us = frame.dts_us;
    frame_out.duration_us = frame.duration_us;
  }
}

bool present_decision_info_is_complete(
    const VPMacOSNativePresentDecisionInfo& info,
    std::string& error) {
  if (!info.should_present) {
    error = "no presentable frame is ready";
    return false;
  }
  return true;
}

bool copy_snapshot_yuv_package(const vr::RendererDrawSnapshot& snapshot,
                               uint8_t* dst,
                               size_t dst_size,
                               int32_t width,
                               int32_t height,
                               size_t max_track_slots,
                               VPMacOSNativePresentFramePackageInfo* out,
                               std::string& error) {
  if (!dst || !out || width <= 0 || height <= 0 || max_track_slots == 0) {
    error = "invalid snapshot YUV package destination";
    return false;
  }
  fill_present_decision_info_from_snapshot(snapshot, width, height, &out->decision);
  if (!present_decision_info_is_complete(out->decision, error)) {
    return false;
  }

  size_t cursor = 0;
  for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
    if (!snapshot.decision.frames[slot].has_value()) {
      continue;
    }
    if (slot >= max_track_slots) {
      error = "snapshot YUV package destination is too small";
      return false;
    }
    const auto& frame = *snapshot.decision.frames[slot];
    const auto* nv12_storage = frame.cpu_nv12_storage();
    const auto* planar_storage = frame.cpu_planar_yuv_storage();
    const auto* cv_storage = frame.macos_cv_pixel_buffer_storage();
    const uint8_t* y_source = nullptr;
    const uint8_t* uv_source = nullptr;
    const uint8_t* v_source = nullptr;
    int y_stride = 0;
    int uv_stride = 0;
    int v_stride = 0;
    int coded_width = 0;
    int coded_height = 0;
    int chroma_width = 0;
    int chroma_height = 0;
    bool is_p010 = false;
    bool is_planar_yuv420 = false;
    std::unique_ptr<ScopedCVPixelBufferLock> cv_lock;
    if (nv12_storage) {
      if (!nv12_storage->data || nv12_storage->y_stride <= 0 ||
          nv12_storage->uv_stride <= 0 || nv12_storage->coded_width <= 0 ||
          nv12_storage->coded_height <= 0 ||
          nv12_storage->coded_width < frame.width ||
          nv12_storage->coded_height < frame.height) {
        error = "snapshot contains invalid NV12/P010 frame storage";
        return false;
      }
      y_source = nv12_storage->data->data();
      y_stride = nv12_storage->y_stride;
      uv_stride = nv12_storage->uv_stride;
      coded_width = nv12_storage->coded_width;
      coded_height = nv12_storage->coded_height;
      is_p010 = nv12_storage->is_p010;
      uv_source = y_source + static_cast<size_t>(y_stride) * coded_height;
      chroma_width = (coded_width + 1) / 2;
      chroma_height = (coded_height + 1) / 2;
    } else if (planar_storage) {
      if (planar_storage->bytes_per_sample != 1) {
        error = "planar 10-bit YUV is not supported by Metal presentation yet";
        return false;
      }
      for (int plane = 0; plane < 3; ++plane) {
        if (!planar_storage->planes[plane] ||
            planar_storage->plane_widths[plane] <= 0 ||
            planar_storage->plane_heights[plane] <= 0 ||
            planar_storage->strides[plane] <
                planar_storage->plane_widths[plane] *
                    planar_storage->bytes_per_sample) {
          error = "snapshot contains invalid planar YUV frame storage";
          return false;
        }
      }
      const int expected_chroma_width =
          (planar_storage->plane_widths[0] + 1) / 2;
      const int expected_chroma_height =
          (planar_storage->plane_heights[0] + 1) / 2;
      if (planar_storage->plane_widths[0] < frame.width ||
          planar_storage->plane_heights[0] < frame.height ||
          planar_storage->plane_widths[1] != expected_chroma_width ||
          planar_storage->plane_widths[2] != expected_chroma_width ||
          planar_storage->plane_heights[1] != expected_chroma_height ||
          planar_storage->plane_heights[2] != expected_chroma_height ||
          planar_storage->strides[1] != planar_storage->strides[2]) {
        error = "snapshot planar YUV storage is not YUV420-compatible";
        return false;
      }
      y_source = planar_storage->planes[0];
      uv_source = planar_storage->planes[1];
      v_source = planar_storage->planes[2];
      y_stride = planar_storage->strides[0];
      uv_stride = planar_storage->strides[1];
      v_stride = planar_storage->strides[2];
      coded_width = planar_storage->plane_widths[0];
      coded_height = planar_storage->plane_heights[0];
      chroma_width = planar_storage->plane_widths[1];
      chroma_height = planar_storage->plane_heights[1];
      is_planar_yuv420 = true;
    } else if (cv_storage) {
      auto* pixel_buffer = static_cast<CVPixelBufferRef>(cv_storage->pixel_buffer);
      cv_lock = std::make_unique<ScopedCVPixelBufferLock>(pixel_buffer);
      if (!pixel_buffer || !cv_lock->locked() || cv_storage->plane_count < 2 ||
          cv_storage->coded_width <= 0 || cv_storage->coded_height <= 0 ||
          cv_storage->coded_width < frame.width ||
          cv_storage->coded_height < frame.height) {
        error = "snapshot contains invalid CVPixelBuffer frame storage";
        return false;
      }
      y_source = static_cast<const uint8_t*>(
          CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
      uv_source = static_cast<const uint8_t*>(
          CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
      y_stride = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0));
      uv_stride = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1));
      coded_width = cv_storage->coded_width;
      coded_height = cv_storage->coded_height;
      is_p010 = cv_storage->is_p010;
      chroma_width = (coded_width + 1) / 2;
      chroma_height = (coded_height + 1) / 2;
    } else {
      error = "snapshot contains unsupported YUV frame storage";
      return false;
    }
    if (!y_source || !uv_source || y_stride <= 0 || uv_stride <= 0) {
      error = "snapshot contains invalid YUV plane pointers";
      return false;
    }

    const int bytes_per_sample = is_p010 ? 2 : 1;
    if (!is_planar_yuv420 &&
        (y_stride < coded_width * bytes_per_sample ||
         uv_stride < coded_width * bytes_per_sample)) {
      error = "invalid NV12/P010 frame storage for Metal presentation";
      return false;
    }
    if (is_planar_yuv420 &&
        (!v_source || v_stride <= 0 ||
         y_stride < coded_width * bytes_per_sample ||
         uv_stride < chroma_width * bytes_per_sample ||
         v_stride < chroma_width * bytes_per_sample)) {
      error = "invalid planar YUV420 frame storage for Metal presentation";
      return false;
    }
    const size_t y_bytes =
        static_cast<size_t>(y_stride) * static_cast<size_t>(coded_height);
    const size_t uv_bytes =
        static_cast<size_t>(uv_stride) * static_cast<size_t>(chroma_height);
    const size_t v_bytes = is_planar_yuv420
        ? static_cast<size_t>(v_stride) * static_cast<size_t>(chroma_height)
        : 0u;
    if (nv12_storage &&
        (y_bytes > std::numeric_limits<size_t>::max() - uv_bytes ||
         y_bytes + uv_bytes > nv12_storage->data->size())) {
      error = "invalid NV12/P010 frame storage for Metal presentation";
      return false;
    }
    cursor = align_up_size(cursor, static_cast<size_t>(bytes_per_sample));
    if (cursor > dst_size || y_bytes > dst_size - cursor ||
        uv_bytes > dst_size - cursor - y_bytes ||
        v_bytes > dst_size - cursor - y_bytes - uv_bytes) {
      error = "snapshot YUV package destination is too small";
      return false;
    }
    std::memcpy(dst + cursor, y_source, y_bytes);
    out->decision.y_offset[slot] = static_cast<int32_t>(cursor);
    cursor += y_bytes;
    std::memcpy(dst + cursor, uv_source, uv_bytes);
    out->decision.uv_offset[slot] = static_cast<int32_t>(cursor);
    cursor += uv_bytes;
    if (is_planar_yuv420) {
      std::memcpy(dst + cursor, v_source, v_bytes);
      out->decision.v_offset[slot] = static_cast<int32_t>(cursor);
      cursor += v_bytes;
    }
    out->decision.yuv_format[slot] = is_planar_yuv420
        ? VPMacOSNativePresentFormatYUV420P
        : (is_p010 ? VPMacOSNativePresentFormatP010
                   : VPMacOSNativePresentFormatNV12);
    out->decision.y_stride[slot] = y_stride;
    out->decision.uv_stride[slot] = uv_stride;
    out->decision.coded_width[slot] = coded_width;
    out->decision.coded_height[slot] = coded_height;
    out->decision.nv12_uv_scale_x[slot] =
        static_cast<float>(frame.width) / static_cast<float>(coded_width);
    out->decision.nv12_uv_scale_y[slot] =
        static_cast<float>(frame.height) / static_cast<float>(coded_height);
  }
  out->used_bytes = cursor;
  return true;
}

bool copy_snapshot_bgra_package(const vr::RendererDrawSnapshot& snapshot,
                                uint8_t* dst,
                                size_t dst_size,
                                int32_t width,
                                int32_t height,
                                int32_t stride_bytes,
                                size_t track_stride_bytes,
                                VPMacOSNativePresentFramePackageInfo* out,
                                std::string& error) {
  if (!dst || !out || width <= 0 || height <= 0 ||
      stride_bytes < width * 4 || track_stride_bytes == 0) {
    error = "invalid snapshot BGRA package destination";
    return false;
  }
  fill_present_decision_info_from_snapshot(snapshot, width, height, &out->decision);
  if (!present_decision_info_is_complete(out->decision, error)) {
    return false;
  }
  size_t required_tracks = 1;
  for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
    if (snapshot.decision.frames[slot].has_value()) {
      required_tracks = std::max(required_tracks, slot + 1);
    }
  }
  if (track_stride_bytes > std::numeric_limits<size_t>::max() / required_tracks ||
      dst_size < track_stride_bytes * required_tracks) {
    error = "snapshot BGRA package destination is too small";
    return false;
  }
  for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
    if (!snapshot.decision.frames[slot].has_value()) {
      continue;
    }
    const auto& frame = *snapshot.decision.frames[slot];
    auto* slot_dst = dst + track_stride_bytes * slot;
    VPMacOSNativeFrameInfo frame_info = {};
    const auto status = copy_texture_frame_to_bgra_destination_checked(
        frame,
        slot_dst,
        track_stride_bytes,
        frame.width,
        frame.height,
        stride_bytes,
        &frame_info);
    if (status != PresentationAdapterStatus::Ok) {
      error = presentation_adapter_status_message(status);
      return false;
    }
  }
  out->used_bytes = track_stride_bytes * required_tracks;
  return true;
}

}  // namespace vp_macos
