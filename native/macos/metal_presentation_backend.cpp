#include "macos/metal_presentation_backend.h"

#include <algorithm>
#include <cstring>
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
  width_ = 0;
  height_ = 0;
  headless_ = true;
}

bool MetalPresentationBackend::available() const {
  return uploader_ && VPMacOSMetalUploaderIsAvailable(uploader_) != 0;
}

}  // namespace vp_macos

struct VPMacOSMetalPresentationBackend {
  vp_macos::MetalPresentationBackend impl;
};

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

int64_t VPMacOSMetalPresentationBackendDirectYUVUploadCount(
    VPMacOSMetalPresentationBackend* backend) {
  return VPMacOSMetalUploaderDirectYUVUploadCount(
      VPMacOSMetalPresentationBackendUploader(backend));
}

int64_t VPMacOSMetalPresentationBackendPresentPackageUploadCount(
    VPMacOSMetalPresentationBackend* backend) {
  return VPMacOSMetalUploaderPresentPackageUploadCount(
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

  const size_t package_size =
      VPMacOSNativePresentFramePackageMaxBytes(width, height, max_track_slots);
  if (package_size == 0) {
    if (error && error_size > 0) {
      const char* message = "native Metal presentation package dimensions overflow";
      const size_t copy_size = std::min(error_size - 1, std::strlen(message));
      std::memcpy(error, message, copy_size);
      error[copy_size] = '\0';
    }
    return -1;
  }

  std::vector<uint8_t> package_data(package_size);
  VPMacOSNativePresentFramePackageInfo package = {};
  const int copy_ret = VPMacOSNativePlayerCopyPresentFramePackage(
      player,
      package_data.data(),
      package_data.size(),
      width,
      height,
      max_track_slots,
      &package,
      error,
      error_size);
  if (copy_ret != 0) {
    if (error && std::strcmp(error, "not all present decision frames are ready") == 0) {
      return -1;
    }
    return -2;
  }

  return VPMacOSMetalPresentationBackendCopyPresentFramePackageWithLayout(
      backend,
      package_data.data(),
      package_data.size(),
      &package,
      pixel_buffer,
      width,
      height,
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
