#pragma once

#include "renderer/render/presentation_backend.h"
#include "windows/d3d11/shared_fp16_ring.h"
#include "windows/wgpu/wgpu_d3d12_ffi_bridge.h"

#include <functional>
#include <memory>
#include <string>

namespace vr {

class WgpuD3D12SharedFp16Ring;

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
    PresentationBackendDiagnostics diagnostics() const override;
    const char* last_error() const override { return last_error_.c_str(); }
    bool draw_frame(const RendererDrawSnapshot& snapshot,
                    const PresentationBackendDrawHooks& hooks) override;

private:
    bool headless_ = false;
    VPWgpuD3D12Renderer* renderer_ = nullptr;
    VPWgpuD3D12RendererInfo renderer_info_{};
    std::unique_ptr<WgpuD3D12SharedFp16Ring> shared_fp16_ring_;
    std::function<void()> shared_fp16_callback_;
    std::string last_error_ = "wgpu-d3d12 presentation backend is not initialized";
};

} // namespace vr
