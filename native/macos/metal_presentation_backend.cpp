#include "macos/metal_presentation_backend.h"

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
