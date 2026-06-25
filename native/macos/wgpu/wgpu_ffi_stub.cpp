#include "macos/wgpu/wgpu_ffi_bridge.h"

#ifndef VOIDPLAYER_WGPU_RUST_LINKED

#include <algorithm>
#include <cstring>

namespace {

void write_error(char* error, size_t error_size, const char* message) {
  if (!error || error_size == 0) {
    return;
  }
  const char* text = message ? message : "";
  const size_t copy_size = std::min(error_size - 1, std::strlen(text));
  std::memcpy(error, text, copy_size);
  error[copy_size] = '\0';
}

}  // namespace

extern "C" __attribute__((weak)) int VPWgpuFfiVersion(void) {
  return 0;
}

extern "C" __attribute__((weak)) VPWgpuMetalRenderer* VPWgpuMetalRendererCreate(
    char* error,
    size_t error_size) {
  write_error(error, error_size, "wgpu-metal Rust FFI is not linked");
  return nullptr;
}

extern "C" __attribute__((weak)) void VPWgpuMetalRendererDestroy(
    VPWgpuMetalRenderer*) {}

extern "C" __attribute__((weak)) int VPWgpuMetalRendererGetInfo(
    VPWgpuMetalRenderer*,
    VPWgpuMetalRendererInfo* info) {
  if (info) {
    std::memset(info, 0, sizeof(*info));
    write_error(info->adapter_description,
                sizeof(info->adapter_description),
                "wgpu-metal Rust FFI is not linked");
    write_error(info->driver_type, sizeof(info->driver_type), "unlinked");
    write_error(info->backend, sizeof(info->backend), "none");
    write_error(info->device_type, sizeof(info->device_type), "unknown");
  }
  return -1;
}

extern "C" __attribute__((weak)) int VPWgpuMetalRendererGetProfilerSnapshot(
    VPWgpuMetalRenderer*,
    VPWgpuMetalProfilerSnapshot* snapshot) {
  if (snapshot) {
    std::memset(snapshot, 0, sizeof(*snapshot));
  }
  return -1;
}

extern "C" __attribute__((weak)) int VPWgpuMetalRendererRenderPackage(
    VPWgpuMetalRenderer*,
    const VPWgpuMetalRenderRequest* request) {
  if (request) {
    write_error(request->error,
                request->error_size,
                "wgpu-metal Rust FFI is not linked");
  }
  return -1;
}

extern "C" __attribute__((weak)) int VPWgpuMetalRendererRenderPackageAsync(
    VPWgpuMetalRenderer*,
    const VPWgpuMetalRenderRequest* request,
    VPWgpuMetalAsyncCompletion) {
  if (request) {
    write_error(request->error,
                request->error_size,
                "wgpu-metal Rust FFI is not linked");
  }
  return -1;
}

extern "C" __attribute__((weak)) int
VPWgpuMetalRendererRenderCVPixelBufferFrameSet(
    VPWgpuMetalRenderer*,
    const VPWgpuMetalCVPixelBufferRenderRequest* request) {
  if (request) {
    write_error(request->error,
                request->error_size,
                "wgpu-metal Rust FFI is not linked");
  }
  return -1;
}

extern "C" __attribute__((weak)) int
VPWgpuMetalRendererRenderCVPixelBufferFrameSetAsync(
    VPWgpuMetalRenderer*,
    const VPWgpuMetalCVPixelBufferRenderRequest* request,
    VPWgpuMetalAsyncCompletion) {
  if (request) {
    write_error(request->error,
                request->error_size,
                "wgpu-metal Rust FFI is not linked");
  }
  return -1;
}

extern "C" __attribute__((weak)) int
VPWgpuMetalRendererCompositeRetainedSource(
    VPWgpuMetalRenderer*,
    const VPWgpuMetalRetainedCompositeRequest* request) {
  if (request) {
    write_error(request->error,
                request->error_size,
                "wgpu-metal Rust FFI is not linked");
  }
  return -1;
}

extern "C" __attribute__((weak)) int
VPWgpuMetalRendererCompositeRetainedSourceAsync(
    VPWgpuMetalRenderer*,
    const VPWgpuMetalRetainedCompositeRequest* request,
    VPWgpuMetalAsyncCompletion) {
  if (request) {
    write_error(request->error,
                request->error_size,
                "wgpu-metal Rust FFI is not linked");
  }
  return -1;
}

extern "C" __attribute__((weak)) int VPWgpuMetalRenderPackage(
    const VPWgpuMetalRenderRequest* request) {
  if (request) {
    write_error(request->error,
                request->error_size,
                "wgpu-metal Rust FFI is not linked");
  }
  return -1;
}

#endif  // VOIDPLAYER_WGPU_RUST_LINKED
