#pragma once

#include "renderer/render/presentation_backend.h"
#include "windows/shared/shared_texture_ring_types.h"
#include "windows/wgpu/wgpu_d3d12_ffi_bridge.h"

#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

namespace vr {

class WgpuD3D12SharedFp16Ring;
class WgpuD3D12SharedSourceCacheRing;

class WgpuD3D12PresentationBackend final : public PresentationBackend {
public:
    WgpuD3D12PresentationBackend();
    ~WgpuD3D12PresentationBackend() override;

    PresentationBackendKind kind() const override {
        return PresentationBackendKind::WgpuD3D12;
    }
    const char* name() const override { return "wgpu-d3d12"; }
    bool initialize(const PresentationBackendConfig& config) override;
    void shutdown() override;
    bool headless() const override { return headless_; }
    void* native_render_device() const override;
    bool acquire_shared_fp16_texture(SharedFp16TextureSnapshot& snapshot) override;
    void release_shared_fp16_texture(int buffer_index,
                                     uint64_t ring_generation) override;
    void set_shared_fp16_frame_callback(std::function<void()> callback) override;
    bool update_external_flutter_surface(
        const PresentationExternalD3D12Surface& surface) override;
    void clear_external_flutter_surface() override;
    bool configure_source_cache(
        const std::vector<SourceCacheTrackDescriptor>& descriptors) override;
    void clear_source_cache(const char* reason) override;
    bool acquire_source_cache_bundle(
        SharedSourceCacheBundleSnapshot& snapshot) override;
    void release_source_cache_bundle(int buffer_index,
                                     uint64_t ring_generation) override;
    void set_source_cache_frame_callback(std::function<void()> callback) override;
    bool capture_front_buffer(std::vector<uint8_t>& bgra,
                              int& width,
                              int& height) override;
    bool capture_front_buffer_region(int x,
                                     int y,
                                     int width,
                                     int height,
                                     std::vector<uint8_t>& bgra,
                                     int& out_width,
                                     int& out_height) override;
    PresentationBackendDiagnostics diagnostics() const override;
    const char* last_error() const override { return last_error_.c_str(); }
    bool draw_frame(const RendererDrawSnapshot& snapshot,
                    const PresentationBackendDrawHooks& hooks) override;

private:
    struct ExternalFlutterSurface {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        int32_t width = 0;
        int32_t height = 0;
        int32_t format = 0;
        int32_t sync = 0;
        uint64_t fence_value = 0;
        uint64_t ring_generation = 0;
        uint64_t frame_generation = 0;
        bool valid = false;
    };

    bool headless_ = false;
    VPWgpuD3D12Renderer* renderer_ = nullptr;
    VPWgpuD3D12RendererInfo renderer_info_{};
    std::unique_ptr<WgpuD3D12SharedFp16Ring> shared_fp16_ring_;
    std::unique_ptr<WgpuD3D12SharedSourceCacheRing> source_cache_ring_;
    std::vector<SourceCacheTrackDescriptor> source_cache_descriptors_;
    std::function<void()> shared_fp16_callback_;
    std::function<void()> source_cache_callback_;
    std::string source_cache_error_ = "none";
    std::string last_error_ = "wgpu-d3d12 presentation backend is not initialized";
    mutable std::mutex external_flutter_mutex_;
    ExternalFlutterSurface external_flutter_;
    uint64_t external_flutter_surface_generation_ = 0;
    uint64_t external_flutter_surface_consumed_generation_ = 0;
    uint64_t external_flutter_surface_update_count_ = 0;
    uint64_t external_flutter_surface_consume_count_ = 0;
    uint64_t external_flutter_surface_wait_count_ = 0;
    uint64_t external_flutter_surface_wait_failure_count_ = 0;
    std::string external_flutter_surface_last_error_ = "none";
};

} // namespace vr
