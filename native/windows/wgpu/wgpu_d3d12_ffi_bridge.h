#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPWgpuD3D12Renderer VPWgpuD3D12Renderer;

enum {
  VP_WGPU_FFI_ABI_VERSION = 11,
};

enum {
  VP_WGPU_D3D12_TEXTURE_FORMAT_NV12 = 1,
  VP_WGPU_D3D12_TEXTURE_FORMAT_P010 = 2,
  VP_WGPU_D3D12_TEXTURE_FORMAT_BGRA8_UNORM = 3,
  VP_WGPU_D3D12_TEXTURE_FORMAT_RGBA16_FLOAT = 4,
};

enum {
  VP_WGPU_D3D12_OUTPUT_COLOR_MODE_SDR = 1,
  VP_WGPU_D3D12_OUTPUT_COLOR_MODE_EDR = 2,
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

typedef struct VPWgpuD3D12RenderTargetClearRequest {
  void* d3d12_resource;
  int32_t format;
  uint32_t width;
  uint32_t height;
  float color[4];
  char* error;
  size_t error_size;
} VPWgpuD3D12RenderTargetClearRequest;

typedef struct VPWgpuD3D12PresentFrameInfo {
  int32_t present;
  int32_t file_id;
  int32_t slot;
  int32_t width;
  int32_t height;
  int64_t pts_us;
  int64_t dts_us;
  int64_t duration_us;
  int32_t analysis_frame_index;
  int32_t frame_identity_mode;
  int32_t source_packet_index;
  int32_t source_packet_size;
  int64_t source_packet_pos;
  int64_t source_packet_pts;
  int64_t source_packet_dts;
  int32_t color_range;
  int32_t color_matrix;
  int32_t color_transfer;
  int32_t color_primaries;
} VPWgpuD3D12PresentFrameInfo;

typedef struct VPWgpuD3D12PresentDecisionInfo {
  int32_t should_present;
  int32_t frame_count;
  int32_t track_count;
  int32_t mode;
  int64_t current_pts_us;
  float split_pos;
  float background_color[4];
  int32_t order[4];
  float display_offset_x[4];
  float display_offset_y[4];
  float inv_display_size_x[4];
  float inv_display_size_y[4];
  float view_offset_uv_x[4];
  float view_offset_uv_y[4];
  int32_t source_width[4];
  int32_t source_height[4];
  int32_t yuv_format[4];
  int32_t y_offset[4];
  int32_t uv_offset[4];
  int32_t v_offset[4];
  int32_t y_stride[4];
  int32_t uv_stride[4];
  int32_t coded_width[4];
  int32_t coded_height[4];
  float nv12_uv_scale_x[4];
  float nv12_uv_scale_y[4];
  int32_t color_range[4];
  int32_t color_matrix[4];
  int32_t color_transfer[4];
  int32_t color_primaries[4];
  VPWgpuD3D12PresentFrameInfo frames[4];
} VPWgpuD3D12PresentDecisionInfo;

typedef struct VPWgpuD3D12CpuSourceInfo {
  const void* y_data;
  size_t y_size;
  const void* uv_data;
  size_t uv_size;
  int32_t format;
  int32_t y_stride;
  int32_t uv_stride;
  uint32_t y_width;
  uint32_t y_height;
  uint32_t uv_width;
  uint32_t uv_height;
} VPWgpuD3D12CpuSourceInfo;

typedef struct VPWgpuD3D12CompositeRequest {
  void* destination_resource;
  int32_t output_format;
  int32_t output_color_mode;
  void* source_resources[4];
  int32_t source_formats[4];
  uint32_t source_array_layers[4];
  uint32_t source_base_array_layers[4];
  VPWgpuD3D12CpuSourceInfo cpu_sources[4];
  const VPWgpuD3D12PresentDecisionInfo* decision;
  int32_t width;
  int32_t height;
  char* error;
  size_t error_size;
} VPWgpuD3D12CompositeRequest;

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
int VPWgpuD3D12RendererClearRenderTargetForProbe(
    VPWgpuD3D12Renderer* renderer,
    const VPWgpuD3D12RenderTargetClearRequest* request);
int VPWgpuD3D12RendererRenderComposite(
    VPWgpuD3D12Renderer* renderer,
    const VPWgpuD3D12CompositeRequest* request);

#ifdef __cplusplus
}  // extern "C"
#endif
