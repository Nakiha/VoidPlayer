#include "macos/metal/metal_presentation_backend.h"

#include "renderer/render/presentation_backend.h"

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

void VPMacOSMetalPresentationBackendSetDrawTargetRing(
    VPMacOSMetalPresentationBackend* backend,
    const void* const* pixel_buffers,
    size_t pixel_buffer_count,
    void* displayed_pixel_buffer,
    void* protected_pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots) {
  if (backend) {
    backend->impl.set_draw_target_ring(pixel_buffers,
                                       pixel_buffer_count,
                                       displayed_pixel_buffer,
                                       protected_pixel_buffer,
                                       width,
                                       height,
                                       max_track_slots);
  }
}

void VPMacOSMetalPresentationBackendClearDrawTarget(
    VPMacOSMetalPresentationBackend* backend) {
  if (backend) {
    backend->impl.clear_draw_target();
  }
}

int VPMacOSMetalPresentationBackendContainsDrawTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer) {
  return backend && backend->impl.contains_draw_target(pixel_buffer) ? 1 : 0;
}

void VPMacOSMetalPresentationBackendMarkDisplayedTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer) {
  if (backend) {
    backend->impl.mark_displayed_target(pixel_buffer);
  }
}

void VPMacOSMetalPresentationBackendProtectTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer) {
  if (backend) {
    backend->impl.protect_target(pixel_buffer);
  }
}

void VPMacOSMetalPresentationBackendReleaseTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer) {
  if (backend) {
    backend->impl.release_target(pixel_buffer);
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
