#include "macos/metal/metal_presentation_backend_bridge.h"

#include "renderer/render/backend_type.h"
#include "renderer/render/presentation_backend.h"
#include "renderer/render/presentation_backend_factory.h"

#include <algorithm>
#include <mutex>
#include <memory>
#include <vector>

struct VPMacOSMetalPresentationBackend {
  explicit VPMacOSMetalPresentationBackend(int32_t initial_width,
                                           int32_t initial_height)
      : uploader(VPMacOSMetalUploaderCreate()),
        width(initial_width),
        height(initial_height) {}

  ~VPMacOSMetalPresentationBackend() {
    VPMacOSMetalUploaderDestroy(uploader);
  }

  VPMacOSMetalUploader* uploader = nullptr;
  std::shared_ptr<vr::PresentationBackend> source_bake_backend;
  std::mutex mutex;
  std::vector<void*> pixel_buffers;
  void* displayed_pixel_buffer = nullptr;
  void* protected_pixel_buffer = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  int32_t max_track_slots = 1;
};

namespace {

void install_single_target_locked(VPMacOSMetalPresentationBackend& backend,
                                  void* pixel_buffer,
                                  int32_t width,
                                  int32_t height,
                                  int32_t max_track_slots) {
  backend.pixel_buffers.clear();
  if (pixel_buffer) {
    backend.pixel_buffers.push_back(pixel_buffer);
  }
  backend.displayed_pixel_buffer = nullptr;
  backend.protected_pixel_buffer = nullptr;
  backend.width = width;
  backend.height = height;
  backend.max_track_slots = max_track_slots;
}

bool contains_target_locked(const VPMacOSMetalPresentationBackend& backend,
                            void* pixel_buffer) {
  return pixel_buffer &&
         std::find(backend.pixel_buffers.begin(),
                   backend.pixel_buffers.end(),
                   pixel_buffer) != backend.pixel_buffers.end();
}

}  // namespace

void write_bridge_error(char* error, size_t error_size, const char* message) {
  if (!error || error_size == 0) {
    return;
  }
  const char* safe_message = message ? message : "";
  size_t index = 0;
  for (; index + 1 < error_size && safe_message[index] != '\0'; ++index) {
    error[index] = safe_message[index];
  }
  error[index] = '\0';
}

VPMacOSMetalPresentationBackend* VPMacOSMetalPresentationBackendCreate(int32_t width,
                                                                       int32_t height) {
  auto* backend = new VPMacOSMetalPresentationBackend(width, height);
  if (!backend->uploader ||
      VPMacOSMetalUploaderIsAvailable(backend->uploader) == 0) {
    delete backend;
    return nullptr;
  }
  return backend;
}

void VPMacOSMetalPresentationBackendDestroy(VPMacOSMetalPresentationBackend* backend) {
  delete backend;
}

int VPMacOSMetalPresentationBackendIsAvailable(VPMacOSMetalPresentationBackend* backend) {
  return backend && backend->uploader &&
                 VPMacOSMetalUploaderIsAvailable(backend->uploader) != 0
             ? 1
             : 0;
}

VPMacOSMetalUploader* VPMacOSMetalPresentationBackendUploader(
    VPMacOSMetalPresentationBackend* backend) {
  return backend ? backend->uploader : nullptr;
}

std::shared_ptr<vr::PresentationBackend>
VPMacOSMetalPresentationBackendSourceBakeBackend(
    VPMacOSMetalPresentationBackend* backend,
    void* initial_pixel_buffer,
    int32_t width,
    int32_t height,
    char* error,
    size_t error_size) {
  if (!backend || !initial_pixel_buffer || width <= 0 || height <= 0) {
    write_bridge_error(error, error_size, "invalid source bake backend target");
    return {};
  }
  std::lock_guard<std::mutex> lock(backend->mutex);
  if (backend->source_bake_backend) {
    write_bridge_error(error, error_size, "");
    return backend->source_bake_backend;
  }
  auto source_bake_backend =
      vr::create_presentation_backend(vr::RenderBackendKind::WgpuMetal);
  if (!source_bake_backend) {
    write_bridge_error(error, error_size, "wgpu-metal source bake backend is unavailable");
    return {};
  }
  vr::PresentationBackendConfig config;
  config.output = initial_pixel_buffer;
  config.width = width;
  config.height = height;
  config.max_track_slots = 1;
  config.headless = true;
  if (!source_bake_backend->initialize(config)) {
    write_bridge_error(
        error,
        error_size,
        source_bake_backend->last_error());
    return {};
  }
  backend->source_bake_backend = std::move(source_bake_backend);
  write_bridge_error(error, error_size, "");
  return backend->source_bake_backend;
}

void VPMacOSMetalPresentationBackendSetDrawTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots) {
  if (!backend) {
    return;
  }
  std::lock_guard<std::mutex> lock(backend->mutex);
  install_single_target_locked(*backend,
                               pixel_buffer,
                               width,
                               height,
                               max_track_slots);
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
  if (!backend) {
    return;
  }
  std::lock_guard<std::mutex> lock(backend->mutex);
  backend->pixel_buffers.clear();
  backend->pixel_buffers.reserve(pixel_buffer_count);
  for (size_t index = 0; index < pixel_buffer_count; ++index) {
    void* pixel_buffer = const_cast<void*>(pixel_buffers[index]);
    if (pixel_buffer) {
      backend->pixel_buffers.push_back(pixel_buffer);
    }
  }
  backend->displayed_pixel_buffer =
      contains_target_locked(*backend, displayed_pixel_buffer)
          ? displayed_pixel_buffer
          : nullptr;
  backend->protected_pixel_buffer =
      contains_target_locked(*backend, protected_pixel_buffer)
          ? protected_pixel_buffer
          : nullptr;
  backend->width = width;
  backend->height = height;
  backend->max_track_slots = max_track_slots;
}

void VPMacOSMetalPresentationBackendClearDrawTarget(
    VPMacOSMetalPresentationBackend* backend) {
  if (!backend) {
    return;
  }
  std::lock_guard<std::mutex> lock(backend->mutex);
  backend->pixel_buffers.clear();
  backend->displayed_pixel_buffer = nullptr;
  backend->protected_pixel_buffer = nullptr;
}

int VPMacOSMetalPresentationBackendContainsDrawTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer) {
  if (!backend) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(backend->mutex);
  return contains_target_locked(*backend, pixel_buffer) ? 1 : 0;
}

void VPMacOSMetalPresentationBackendMarkDisplayedTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer) {
  if (!backend) {
    return;
  }
  std::lock_guard<std::mutex> lock(backend->mutex);
  backend->displayed_pixel_buffer =
      contains_target_locked(*backend, pixel_buffer) ? pixel_buffer : nullptr;
}

void VPMacOSMetalPresentationBackendProtectTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer) {
  if (!backend) {
    return;
  }
  std::lock_guard<std::mutex> lock(backend->mutex);
  backend->protected_pixel_buffer =
      contains_target_locked(*backend, pixel_buffer) ? pixel_buffer : nullptr;
}

void VPMacOSMetalPresentationBackendReleaseTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer) {
  if (!backend || !pixel_buffer) {
    return;
  }
  std::lock_guard<std::mutex> lock(backend->mutex);
  if (backend->displayed_pixel_buffer == pixel_buffer) {
    backend->displayed_pixel_buffer = nullptr;
  }
  if (backend->protected_pixel_buffer == pixel_buffer) {
    backend->protected_pixel_buffer = nullptr;
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
