#include "macos/metal_presentation_backend.h"

#include "macos/presentation_package_builder.h"
#include "video_renderer/render/presentation_package.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vp_macos {
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

void MetalPresentationBackend::set_last_error(std::string error) {
  last_error_ = std::move(error);
}

bool MetalPresentationBackend::draw_frame(
    const vr::RendererDrawSnapshot& snapshot,
    const vr::PresentationBackendDrawHooks& hooks) {
  set_last_error("");
  if (!available() || !draw_target_pixel_buffer_ ||
      draw_target_width_ <= 0 || draw_target_height_ <= 0) {
    last_draw_frame_info_available_ = false;
    set_last_error("renderer-owned Metal presentation target is unavailable");
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
    set_last_error("renderer-owned Metal presentation package layout is invalid");
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
    set_last_error(upload_error[0] ? upload_error : error);
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
                                draw_target_width_,
                                draw_target_height_,
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
                                    draw_target_width_,
                                    draw_target_height_,
                                    package.stride_bytes,
                                    package.track_stride_bytes,
                                    &package,
                                    error)) {
      last_draw_frame_info_available_ = false;
      set_last_error(error);
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
  } else {
    set_last_error(upload_error[0] ? upload_error : error);
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
