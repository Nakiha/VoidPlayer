#ifndef VOIDPLAYER_MACOS_WGPU_FFI_BRIDGE_H_
#define VOIDPLAYER_MACOS_WGPU_FFI_BRIDGE_H_

#include "macos/metal/metal_uploader_bridge.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPWgpuMetalRenderer VPWgpuMetalRenderer;

enum {
  VP_WGPU_FFI_ABI_VERSION = 10,
};

enum {
  VP_WGPU_METAL_OUTPUT_FORMAT_BGRA8_UNORM = 1,
  VP_WGPU_METAL_OUTPUT_FORMAT_RGBA16_FLOAT = 2,
};

enum {
  VP_WGPU_METAL_OUTPUT_COLOR_MODE_SDR = 1,
  VP_WGPU_METAL_OUTPUT_COLOR_MODE_EDR = 2,
};

typedef struct VPWgpuMetalRendererInfo {
  char adapter_description[128];
  char driver_type[64];
  char backend[32];
  char device_type[32];
  uint32_t vendor_id;
  uint32_t device_id;
  uint32_t supports_texture_format_16bit_norm;
} VPWgpuMetalRendererInfo;

typedef struct VPWgpuMetalProfilerSnapshot {
  uint64_t destination_import_count;
  uint64_t destination_import_reuse_count;
  uint64_t source_import_count;
  uint64_t source_import_reuse_count;
  uint64_t imported_texture_cache_size;
  uint64_t imported_texture_cache_eviction_count;
  uint64_t final_bind_group_create_count;
  uint64_t overlay_bind_group_create_count;
  uint64_t overlay_layer_rebuild_count;
  uint64_t overlay_layer_reuse_count;
  uint64_t package_buffer_write_count;
  uint64_t params_buffer_write_count;
  uint64_t overlay_buffer_write_count;
  uint64_t submit_count;
  uint64_t last_import_us;
  uint64_t last_prepare_us;
  uint64_t last_overlay_encode_us;
  uint64_t last_bind_group_us;
  uint64_t last_pass_encode_us;
  uint64_t last_submit_us;
  uint64_t last_cpu_render_us;
} VPWgpuMetalProfilerSnapshot;

typedef struct VPWgpuMetalRenderRequest {
  void* destination_mtl_texture;
  int32_t output_format;
  int32_t output_color_mode;
  const uint8_t* package_data;
  size_t package_data_size;
  const VPMacOSNativePresentFramePackageInfo* package;
  const VPMacOSNativeOverlayGpuRect* overlay_fill_rects;
  size_t overlay_fill_rect_count;
  const VPMacOSNativeOverlayGpuRect* overlay_line_rects;
  size_t overlay_line_rect_count;
  const VPMacOSNativeOverlayGpuRect* overlay_motion_lines;
  size_t overlay_motion_line_count;
  uint64_t overlay_generation;
  int32_t width;
  int32_t height;
  char* error;
  size_t error_size;
} VPWgpuMetalRenderRequest;

typedef struct VPWgpuMetalCVPixelBufferRenderRequest {
  void* destination_mtl_texture;
  int32_t output_format;
  int32_t output_color_mode;
  void* source_y_mtl_textures[VPMacOSNativeMaxTracks];
  void* source_uv_mtl_textures[VPMacOSNativeMaxTracks];
  const VPMacOSNativeCVPixelBufferPresentFrameSet* frame_set;
  const VPMacOSNativeOverlayGpuRect* overlay_fill_rects;
  size_t overlay_fill_rect_count;
  const VPMacOSNativeOverlayGpuRect* overlay_line_rects;
  size_t overlay_line_rect_count;
  const VPMacOSNativeOverlayGpuRect* overlay_motion_lines;
  size_t overlay_motion_line_count;
  uint64_t overlay_generation;
  int32_t width;
  int32_t height;
  char* error;
  size_t error_size;
} VPWgpuMetalCVPixelBufferRenderRequest;

typedef struct VPWgpuMetalRetainedCompositeRequest {
  void* destination_mtl_texture;
  int32_t output_format;
  int32_t output_color_mode;
  const VPMacOSNativePresentDecisionInfo* decision;
  const VPMacOSNativeOverlayGpuRect* overlay_fill_rects;
  size_t overlay_fill_rect_count;
  const VPMacOSNativeOverlayGpuRect* overlay_line_rects;
  size_t overlay_line_rect_count;
  const VPMacOSNativeOverlayGpuRect* overlay_motion_lines;
  size_t overlay_motion_line_count;
  uint64_t overlay_generation;
  int32_t width;
  int32_t height;
  char* error;
  size_t error_size;
} VPWgpuMetalRetainedCompositeRequest;

typedef void (*VPWgpuMetalAsyncCompletionCallback)(void* user_data,
                                                   int32_t result);

typedef struct VPWgpuMetalAsyncCompletion {
  VPWgpuMetalAsyncCompletionCallback callback;
  void* user_data;
  VPWgpuMetalProfilerSnapshot* profiler_snapshot;
} VPWgpuMetalAsyncCompletion;

int VPWgpuFfiVersion(void);
VPWgpuMetalRenderer* VPWgpuMetalRendererCreate(char* error, size_t error_size);
void VPWgpuMetalRendererDestroy(VPWgpuMetalRenderer* renderer);
int VPWgpuMetalRendererGetInfo(VPWgpuMetalRenderer* renderer,
                               VPWgpuMetalRendererInfo* info);
int VPWgpuMetalRendererGetProfilerSnapshot(
    VPWgpuMetalRenderer* renderer,
    VPWgpuMetalProfilerSnapshot* snapshot);
void* VPWgpuMetalRendererMetalDevice(VPWgpuMetalRenderer* renderer);
int VPWgpuMetalRendererRenderPackage(VPWgpuMetalRenderer* renderer,
                                     const VPWgpuMetalRenderRequest* request);
int VPWgpuMetalRendererRenderPackageAsync(
    VPWgpuMetalRenderer* renderer,
    const VPWgpuMetalRenderRequest* request,
    VPWgpuMetalAsyncCompletion completion);
int VPWgpuMetalRendererRenderCVPixelBufferFrameSet(
    VPWgpuMetalRenderer* renderer,
    const VPWgpuMetalCVPixelBufferRenderRequest* request);
int VPWgpuMetalRendererRenderCVPixelBufferFrameSetAsync(
    VPWgpuMetalRenderer* renderer,
    const VPWgpuMetalCVPixelBufferRenderRequest* request,
    VPWgpuMetalAsyncCompletion completion);
int VPWgpuMetalRendererCompositeRetainedSource(
    VPWgpuMetalRenderer* renderer,
    const VPWgpuMetalRetainedCompositeRequest* request);
int VPWgpuMetalRendererCompositeRetainedSourceAsync(
    VPWgpuMetalRenderer* renderer,
    const VPWgpuMetalRetainedCompositeRequest* request,
    VPWgpuMetalAsyncCompletion completion);
int VPWgpuMetalRenderPackage(const VPWgpuMetalRenderRequest* request);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VOIDPLAYER_MACOS_WGPU_FFI_BRIDGE_H_
