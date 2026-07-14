#pragma once

#include "renderer/render/renderer_draw_snapshot.h"
#include "renderer/render/presentation_backend_types.h"
#include "renderer/render/presentation_snapshot.h"
#include "renderer/overlay/analysis_overlay_gpu_geometry.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace vr {

struct WindowsD3D11ViewportRendererStats {
  uint64_t draw_count = 0;
  uint64_t hardware_frame_count = 0;
  uint64_t software_frame_count = 0;
  uint64_t resource_rebuild_count = 0;
  uint64_t video_source_update_count = 0;
  uint64_t source_frame_cache_hit_count = 0;
  uint64_t source_frame_cache_miss_count = 0;
  uint64_t overlay_draw_count = 0;
  uint64_t overlay_failure_count = 0;
  uint64_t overlay_last_vertex_count = 0;
  uint64_t overlay_last_fill_rect_count = 0;
  uint64_t overlay_last_line_rect_count = 0;
};

// Consumes the platform-neutral presentation snapshot and writes one complete,
// opaque viewport into a runner-owned D3D11 target. It owns only transient
// shader/upload resources; target allocation and final window composition stay
// in the Windows runner.
class WindowsD3D11ViewportRenderer final {
 public:
  WindowsD3D11ViewportRenderer() = default;
  ~WindowsD3D11ViewportRenderer() = default;

  WindowsD3D11ViewportRenderer(const WindowsD3D11ViewportRenderer&) = delete;
  WindowsD3D11ViewportRenderer& operator=(
      const WindowsD3D11ViewportRenderer&) = delete;

  bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
  void shutdown();

  bool draw(const RendererDrawSnapshot& draw_snapshot,
            const PresentationSnapshot& presentation,
            ID3D11RenderTargetView* target,
            ColorOutputTarget output_target,
            double sdr_white_level_nits);
  bool draw_overlay(const AnalysisOverlayPrimitivePackage& package,
                    const PresentationSnapshot& presentation,
                    ID3D11RenderTargetView* target,
                    ColorOutputTarget output_target,
                    double sdr_white_level_nits);

  const std::string& last_error() const { return last_error_; }
  WindowsD3D11ViewportRendererStats stats() const { return stats_; }

 private:
  struct PlaneResource {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    int width = 0;
    int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  };

  struct TrackResources {
    PlaneResource rgba;
    PlaneResource y;
    PlaneResource uv;
    PlaneResource u;
    PlaneResource v;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hardware_copy;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> hardware_y_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> hardware_uv_srv;
    int hardware_width = 0;
    int hardware_height = 0;
    DXGI_FORMAT hardware_format = DXGI_FORMAT_UNKNOWN;
    std::shared_ptr<void> cached_hardware_frame_ref;
    ID3D11Texture2D* cached_hardware_source = nullptr;
    int cached_hardware_array_index = -1;
    int64_t cached_hardware_pts_us = 0;
    int64_t cached_hardware_dts_us = 0;
  };

  bool create_pipeline();
  bool create_overlay_pipeline();
  bool ensure_overlay_vertex_buffer(size_t vertex_count);
  bool prepare_track(size_t slot,
                     const TextureFrame& frame,
                     ShaderConstants& constants);
  bool prepare_hardware_track(size_t slot,
                              const TextureFrame& frame,
                              const WindowsD3D11FrameStorage& storage,
                              ShaderConstants& constants);
  bool prepare_cpu_rgba_track(size_t slot,
                              const TextureFrame& frame,
                              const CpuRgbaFrameStorage& storage,
                              ShaderConstants& constants);
  bool prepare_cpu_nv12_track(size_t slot,
                              const CpuNv12FrameStorage& storage,
                              ShaderConstants& constants);
  bool prepare_cpu_planar_track(size_t slot,
                                const CpuPlanarYuvFrameStorage& storage,
                                ShaderConstants& constants);
  bool ensure_plane(PlaneResource& resource,
                    int width,
                    int height,
                    DXGI_FORMAT format);
  bool upload_plane(PlaneResource& resource,
                    const uint8_t* data,
                    int stride,
                    int bytes_per_sample,
                    bool shift_10_bit_to_msb = false);
  bool ensure_hardware_copy(TrackResources& resources,
                            ID3D11Texture2D* source);
  bool create_plane_srvs(ID3D11Texture2D* texture,
                         DXGI_FORMAT format,
                         Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& y,
                         Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& uv);
  void clear_track_bindings(size_t slot, ShaderConstants& constants);
  void unbind_shader_resources();
  bool fail(std::string error);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> overlay_vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> overlay_pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> overlay_input_layout_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> overlay_vertex_buffer_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> overlay_constant_buffer_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> overlay_blend_state_;
  size_t overlay_vertex_capacity_ = 0;
  std::array<TrackResources, kMaxTracks> tracks_{};
  std::array<ID3D11ShaderResourceView*, kMaxTracks> rgba_srvs_{};
  std::array<ID3D11ShaderResourceView*, kMaxTracks> y_srvs_{};
  std::array<ID3D11ShaderResourceView*, kMaxTracks> uv_srvs_{};
  std::array<ID3D11ShaderResourceView*, kMaxTracks> u_srvs_{};
  std::array<ID3D11ShaderResourceView*, kMaxTracks> v_srvs_{};
  WindowsD3D11ViewportRendererStats stats_{};
  uint64_t last_layout_revision_ = 0;
  uint64_t layout_log_count_ = 0;
  std::string last_error_;
};

}  // namespace vr
