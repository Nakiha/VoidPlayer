#include "macos/metal_presentation_backend.h"

#include "macos/presentation_adapter.h"
#include "video_renderer/render/presentation_package.h"
#include "video_renderer/render/presentation_snapshot.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace vp_macos {
namespace {

size_t align_up_size(size_t value, size_t alignment) {
  if (alignment <= 1) {
    return value;
  }
  const size_t mask = alignment - 1;
  return (value + mask) & ~mask;
}

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

bool present_decision_is_complete(const VPMacOSNativePresentDecisionInfo& info,
                                  std::string& error) {
  if (!info.should_present) {
    error = "no presentable frame is ready";
    return false;
  }
  if (info.track_count > 1 && info.frame_count < info.track_count) {
    error = "not all present decision frames are ready";
    return false;
  }
  return true;
}

bool copy_snapshot_yuv_package(const vr::RendererDrawSnapshot& snapshot,
                               uint8_t* dst,
                               size_t dst_size,
                               size_t max_track_slots,
                               VPMacOSNativePresentFramePackageInfo* out,
                               std::string& error) {
  if (!dst || !out || max_track_slots == 0) {
    error = "invalid snapshot YUV package destination";
    return false;
  }
  if (!present_decision_is_complete(out->decision, error)) {
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
    } else {
      error = "snapshot contains unsupported YUV frame storage";
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
                                int32_t stride_bytes,
                                size_t track_stride_bytes,
                                VPMacOSNativePresentFramePackageInfo* out,
                                std::string& error) {
  if (!dst || !out || stride_bytes <= 0 || track_stride_bytes == 0) {
    error = "invalid snapshot BGRA package destination";
    return false;
  }
  if (!present_decision_is_complete(out->decision, error)) {
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

bool snapshot_cv_pixel_buffer_frame(const vr::RendererDrawSnapshot& snapshot,
                                    int32_t width,
                                    int32_t height,
                                    VPMacOSNativeCVPixelBufferPresentFrame* out,
                                    std::string& error) {
  if (!out || width <= 0 || height <= 0) {
    error = "invalid CVPixelBuffer snapshot output";
    return false;
  }
  *out = {};
  fill_present_decision_info_from_snapshot(snapshot, width, height, &out->decision);
  if (!present_decision_is_complete(out->decision, error)) {
    return false;
  }
  if (out->decision.frame_count != 1) {
    error = "snapshot is not a single CVPixelBuffer frame";
    return false;
  }
  for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
    if (!snapshot.decision.frames[slot].has_value()) {
      continue;
    }
    if (slot != 0) {
      error = "CVPixelBuffer fast path currently requires the primary track slot";
      return false;
    }
    const auto& frame = *snapshot.decision.frames[slot];
    const auto* storage = frame.macos_cv_pixel_buffer_storage();
    if (!storage || !storage->pixel_buffer || storage->plane_count < 2 ||
        storage->coded_width < frame.width || storage->coded_height < frame.height) {
      error = "snapshot does not contain a supported CVPixelBuffer frame";
      return false;
    }
    out->pixel_buffer = storage->pixel_buffer;
    out->pixel_format = static_cast<int32_t>(storage->pixel_format);
    out->plane_count = storage->plane_count;
    out->is_p010 = storage->is_p010 ? 1 : 0;
    out->coded_width = storage->coded_width;
    out->coded_height = storage->coded_height;
    out->decision.yuv_format[slot] = storage->is_p010
        ? VPMacOSNativePresentFormatP010
        : VPMacOSNativePresentFormatNV12;
    out->decision.coded_width[slot] = storage->coded_width;
    out->decision.coded_height[slot] = storage->coded_height;
    out->decision.nv12_uv_scale_x[slot] =
        static_cast<float>(frame.width) / static_cast<float>(storage->coded_width);
    out->decision.nv12_uv_scale_y[slot] =
        static_cast<float>(frame.height) / static_cast<float>(storage->coded_height);
    return true;
  }
  error = "snapshot has no CVPixelBuffer frame";
  return false;
}

}  // namespace

MetalPresentationBackend::~MetalPresentationBackend() {
  shutdown();
}

bool MetalPresentationBackend::initialize(const vr::PresentationBackendConfig& config) {
  shutdown();
  width_ = config.width;
  height_ = config.height;
  headless_ = config.headless;
  set_draw_target(config.output,
                  config.width,
                  config.height,
                  config.max_track_slots);
  if (width_ <= 0 || height_ <= 0) {
    return false;
  }
  uploader_ = VPMacOSMetalUploaderCreate();
  return available();
}

void MetalPresentationBackend::shutdown() {
  if (uploader_) {
    VPMacOSMetalUploaderDestroy(uploader_);
    uploader_ = nullptr;
  }
  clear_draw_target();
  width_ = 0;
  height_ = 0;
  headless_ = true;
}

bool MetalPresentationBackend::available() const {
  return uploader_ && VPMacOSMetalUploaderIsAvailable(uploader_) != 0;
}

bool MetalPresentationBackend::draw_frame(
    const vr::RendererDrawSnapshot& snapshot,
    const vr::PresentationBackendDrawHooks& hooks) {
  if (!available() || !draw_target_pixel_buffer_ ||
      draw_target_width_ <= 0 || draw_target_height_ <= 0) {
    last_draw_frame_info_available_ = false;
    return false;
  }

  const int32_t track_slots =
      std::clamp(draw_target_max_track_slots_,
                 1,
                 static_cast<int>(VPMacOSNativeMaxTracks));
  const auto package_layout = vr::describe_presentation_package_layout(
      draw_target_width_, draw_target_height_, track_slots);
  if (package_layout.max_bytes == 0 ||
      package_layout.bgra_row_bytes >
          static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    last_draw_frame_info_available_ = false;
    return false;
  }

  std::string error;
  VPMacOSNativeCVPixelBufferPresentFrame cv_frame = {};
  if (snapshot_cv_pixel_buffer_frame(snapshot,
                                     draw_target_width_,
                                     draw_target_height_,
                                     &cv_frame,
                                     error)) {
    VPMacOSNativeFrameInfo frame_info = {};
    char upload_error[256] = {};
    const auto start = std::chrono::steady_clock::now();
    const int ret = VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayout(
        uploader_,
        &cv_frame,
        draw_target_pixel_buffer_,
        draw_target_width_,
        draw_target_height_,
        &frame_info,
        upload_error,
        sizeof(upload_error));
    if (hooks.record_frame_copy_us) {
      hooks.record_frame_copy_us(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - start).count()));
    }
    last_draw_frame_info_available_ = ret == 0;
    if (ret == 0) {
      last_draw_frame_info_ = frame_info;
      return true;
    }
    return false;
  }

  VPMacOSNativePresentFramePackageInfo package = {};
  package.width = draw_target_width_;
  package.height = draw_target_height_;
  package.max_track_slots = track_slots;
  fill_present_decision_info_from_snapshot(
      snapshot, draw_target_width_, draw_target_height_, &package.decision);

  std::vector<uint8_t> data(package_layout.max_bytes);
  if (copy_snapshot_yuv_package(snapshot,
                                data.data(),
                                data.size(),
                                static_cast<size_t>(track_slots),
                                &package,
                                error)) {
    package.storage = VPMacOSNativePresentPackageStorageYUV;
  } else {
    fill_present_decision_info_from_snapshot(
        snapshot, draw_target_width_, draw_target_height_, &package.decision);
    package.stride_bytes = static_cast<int32_t>(package_layout.bgra_row_bytes);
    package.track_stride_bytes = package_layout.bgra_track_stride_bytes;
    if (!copy_snapshot_bgra_package(snapshot,
                                    data.data(),
                                    data.size(),
                                    package.stride_bytes,
                                    package.track_stride_bytes,
                                    &package,
                                    error)) {
      last_draw_frame_info_available_ = false;
      return false;
    }
    package.storage = VPMacOSNativePresentPackageStorageBGRA;
  }

  VPMacOSNativeFrameInfo frame_info = {};
  char upload_error[256] = {};
  const auto start = std::chrono::steady_clock::now();
  const int ret = VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
      uploader_,
      data.data(),
      package.used_bytes,
      &package,
      draw_target_pixel_buffer_,
      draw_target_width_,
      draw_target_height_,
      &frame_info,
      upload_error,
      sizeof(upload_error));
  if (hooks.record_frame_copy_us) {
    hooks.record_frame_copy_us(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count()));
  }
  last_draw_frame_info_available_ = ret == 0;
  if (ret == 0) {
    last_draw_frame_info_ = frame_info;
  }
  return ret == 0;
}

void MetalPresentationBackend::set_draw_target(void* pixel_buffer,
                                               int32_t width,
                                               int32_t height,
                                               int32_t max_track_slots) {
  draw_target_pixel_buffer_ = pixel_buffer;
  draw_target_width_ = width;
  draw_target_height_ = height;
  draw_target_max_track_slots_ =
      std::clamp(max_track_slots,
                 1,
                 static_cast<int32_t>(VPMacOSNativeMaxTracks));
}

void MetalPresentationBackend::clear_draw_target() {
  draw_target_pixel_buffer_ = nullptr;
  draw_target_width_ = 0;
  draw_target_height_ = 0;
  draw_target_max_track_slots_ = VPMacOSNativeMaxTracks;
  last_draw_frame_info_available_ = false;
  last_draw_frame_info_ = {};
}

bool MetalPresentationBackend::copy_last_draw_frame_info(
    VPMacOSNativeFrameInfo* out) const {
  if (!out || !last_draw_frame_info_available_) {
    return false;
  }
  *out = last_draw_frame_info_;
  return true;
}

int MetalPresentationBackend::copy_current_frame_with_layout(
    VPMacOSNativePlayer* player,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots,
    int32_t wait_timeout_ms,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  if (player) {
    VPMacOSNativeCVPixelBufferPresentFrame cv_frame = {};
    char cv_error[256] = {};
    if (VPMacOSNativePlayerCopyRetainedCVPixelBufferPresentFrame(
            player,
            width,
            height,
            &cv_frame,
            cv_error,
            sizeof(cv_error)) == 0) {
      const int cv_ret = VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayout(
          uploader_,
          &cv_frame,
          pixel_buffer,
          width,
          height,
          out,
          error,
          error_size);
      VPMacOSNativeReleaseRetainedCVPixelBuffer(cv_frame.pixel_buffer);
      if (cv_ret == 0) {
        return 0;
      }
    }
  }

  return VPMacOSMetalUploaderCopyCurrentFrameWithLayout(
      uploader_,
      player,
      pixel_buffer,
      width,
      height,
      max_track_slots,
      wait_timeout_ms,
      out,
      error,
      error_size);
}

}  // namespace vp_macos

VPMacOSMetalPresentationBackend* VPMacOSMetalPresentationBackendCreate(int32_t width,
                                                                       int32_t height) {
  auto* backend = new VPMacOSMetalPresentationBackend();
  vr::PresentationBackendConfig config;
  config.width = width;
  config.height = height;
  config.headless = true;
  if (!backend->impl.initialize(config)) {
    delete backend;
    return nullptr;
  }
  return backend;
}

void VPMacOSMetalPresentationBackendDestroy(VPMacOSMetalPresentationBackend* backend) {
  delete backend;
}

int VPMacOSMetalPresentationBackendIsAvailable(VPMacOSMetalPresentationBackend* backend) {
  return backend && backend->impl.available() ? 1 : 0;
}

VPMacOSMetalUploader* VPMacOSMetalPresentationBackendUploader(
    VPMacOSMetalPresentationBackend* backend) {
  return backend ? backend->impl.uploader() : nullptr;
}

void VPMacOSMetalPresentationBackendSetDrawTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots) {
  if (backend) {
    backend->impl.set_draw_target(pixel_buffer, width, height, max_track_slots);
  }
}

void VPMacOSMetalPresentationBackendClearDrawTarget(
    VPMacOSMetalPresentationBackend* backend) {
  if (backend) {
    backend->impl.clear_draw_target();
  }
}

int64_t VPMacOSMetalPresentationBackendDirectYUVUploadCount(
    VPMacOSMetalPresentationBackend* backend) {
  return VPMacOSMetalUploaderDirectYUVUploadCount(
      VPMacOSMetalPresentationBackendUploader(backend));
}

int64_t VPMacOSMetalPresentationBackendCVPixelBufferUploadCount(
    VPMacOSMetalPresentationBackend* backend) {
  return VPMacOSMetalUploaderCVPixelBufferUploadCount(
      VPMacOSMetalPresentationBackendUploader(backend));
}

int64_t VPMacOSMetalPresentationBackendPresentPackageUploadCount(
    VPMacOSMetalPresentationBackend* backend) {
  return VPMacOSMetalUploaderPresentPackageUploadCount(
      VPMacOSMetalPresentationBackendUploader(backend));
}

int64_t VPMacOSMetalPresentationBackendLastPresentPackageCopyUs(
    VPMacOSMetalPresentationBackend* backend) {
  return VPMacOSMetalUploaderLastPresentPackageCopyUs(
      VPMacOSMetalPresentationBackendUploader(backend));
}

int64_t VPMacOSMetalPresentationBackendLastPresentPackageGpuWaitUs(
    VPMacOSMetalPresentationBackend* backend) {
  return VPMacOSMetalUploaderLastPresentPackageGpuWaitUs(
      VPMacOSMetalPresentationBackendUploader(backend));
}

int64_t VPMacOSMetalPresentationBackendLastPresentPackageTotalUs(
    VPMacOSMetalPresentationBackend* backend) {
  return VPMacOSMetalUploaderLastPresentPackageTotalUs(
      VPMacOSMetalPresentationBackendUploader(backend));
}

int32_t VPMacOSMetalPresentationBackendLastPresentPackageStorage(
    VPMacOSMetalPresentationBackend* backend) {
  return VPMacOSMetalUploaderLastPresentPackageStorage(
      VPMacOSMetalPresentationBackendUploader(backend));
}

int VPMacOSMetalPresentationBackendValidatePixelBufferChecked(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    char* error,
    size_t error_size) {
  return VPMacOSMetalUploaderValidatePixelBufferChecked(
      VPMacOSMetalPresentationBackendUploader(backend),
      pixel_buffer,
      width,
      height,
      error,
      error_size);
}

int VPMacOSMetalPresentationBackendCopyCurrentFrameWithLayout(
    VPMacOSMetalPresentationBackend* backend,
    VPMacOSNativePlayer* player,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots,
    int32_t wait_timeout_ms,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  if (!backend || !player) {
    return VPMacOSMetalUploaderCopyCurrentFrameWithLayout(
        VPMacOSMetalPresentationBackendUploader(backend),
        player,
        pixel_buffer,
        width,
        height,
        max_track_slots,
        wait_timeout_ms,
        out,
        error,
        error_size);
  }
  return backend->impl.copy_current_frame_with_layout(
      player,
      pixel_buffer,
      width,
      height,
      max_track_slots,
      wait_timeout_ms,
      out,
      error,
      error_size);
}

int VPMacOSMetalPresentationBackendCopyPresentFramePackageWithLayout(
    VPMacOSMetalPresentationBackend* backend,
    const uint8_t* data,
    size_t data_size,
    const VPMacOSNativePresentFramePackageInfo* package,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  return VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
      VPMacOSMetalPresentationBackendUploader(backend),
      data,
      data_size,
      package,
      pixel_buffer,
      width,
      height,
      out,
      error,
      error_size);
}

int VPMacOSMetalPresentationBackendCopyCVPixelBufferPresentFrameWithLayout(
    VPMacOSMetalPresentationBackend* backend,
    const VPMacOSNativeCVPixelBufferPresentFrame* frame,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  return VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayout(
      VPMacOSMetalPresentationBackendUploader(backend),
      frame,
      pixel_buffer,
      width,
      height,
      out,
      error,
      error_size);
}
