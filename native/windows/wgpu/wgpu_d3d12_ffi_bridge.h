#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPWgpuD3D12Renderer VPWgpuD3D12Renderer;

enum {
  VP_WGPU_FFI_ABI_VERSION = 10,
};

enum {
  VP_WGPU_D3D12_TEXTURE_FORMAT_NV12 = 1,
  VP_WGPU_D3D12_TEXTURE_FORMAT_P010 = 2,
  VP_WGPU_D3D12_TEXTURE_FORMAT_BGRA8_UNORM = 3,
  VP_WGPU_D3D12_TEXTURE_FORMAT_RGBA16_FLOAT = 4,
};

typedef struct VPWgpuD3D12RendererInfo {
  char adapter_description[128];
  char driver_type[64];
  char backend[32];
  char device_type[32];
  uint32_t vendor_id;
  uint32_t device_id;
  uint32_t supports_nv12;
  uint32_t supports_p010;
  uint32_t supports_rgba16_float;
} VPWgpuD3D12RendererInfo;

typedef struct VPWgpuD3D12ProfilerSnapshot {
  uint64_t destination_import_count;
  uint64_t source_import_count;
  uint64_t submit_count;
  uint64_t last_import_us;
  uint64_t last_prepare_us;
  uint64_t last_pass_encode_us;
  uint64_t last_submit_us;
  uint64_t last_cpu_render_us;
} VPWgpuD3D12ProfilerSnapshot;

typedef struct VPWgpuD3D12TextureImportRequest {
  void* d3d12_resource;
  int32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t array_layers;
  uint32_t mip_levels;
  uint32_t sample_count;
  char* error;
  size_t error_size;
} VPWgpuD3D12TextureImportRequest;

int VPWgpuFfiVersion(void);
VPWgpuD3D12Renderer* VPWgpuD3D12RendererCreate(char* error,
                                               size_t error_size);
void VPWgpuD3D12RendererDestroy(VPWgpuD3D12Renderer* renderer);
int VPWgpuD3D12RendererGetInfo(VPWgpuD3D12Renderer* renderer,
                               VPWgpuD3D12RendererInfo* info);
int VPWgpuD3D12RendererGetProfilerSnapshot(
    VPWgpuD3D12Renderer* renderer,
    VPWgpuD3D12ProfilerSnapshot* snapshot);
void* VPWgpuD3D12RendererD3D12Device(VPWgpuD3D12Renderer* renderer);
int VPWgpuD3D12RendererImportTextureForProbe(
    VPWgpuD3D12Renderer* renderer,
    const VPWgpuD3D12TextureImportRequest* request);

#ifdef __cplusplus
}  // extern "C"
#endif
