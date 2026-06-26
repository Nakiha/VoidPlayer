#include "windows/wgpu/d3d12_presentation_backend.h"

#include <array>
#include <spdlog/spdlog.h>

namespace vr {

WgpuD3D12PresentationBackend::~WgpuD3D12PresentationBackend() {
    shutdown();
}

bool WgpuD3D12PresentationBackend::initialize(
    const PresentationBackendConfig& config) {
    shutdown();
    headless_ = config.headless;
#if VOIDPLAYER_WGPU_RUST_LINKED
    if (VPWgpuFfiVersion() != VP_WGPU_FFI_ABI_VERSION) {
        last_error_ = "wgpu-d3d12 Rust FFI ABI mismatch";
        spdlog::error("[WgpuD3D12] {}", last_error_);
        return false;
    }
    std::array<char, 512> error{};
    renderer_ = VPWgpuD3D12RendererCreate(error.data(), error.size());
    if (!renderer_) {
        last_error_ = error.data()[0] != '\0'
                          ? error.data()
                          : "wgpu-d3d12 renderer creation failed";
        spdlog::error("[WgpuD3D12] {}", last_error_);
        return false;
    }
    if (VPWgpuD3D12RendererGetInfo(renderer_, &renderer_info_) != 0) {
        last_error_ = "wgpu-d3d12 renderer info query failed";
        spdlog::error("[WgpuD3D12] {}", last_error_);
        shutdown();
        return false;
    }
    if (!VPWgpuD3D12RendererD3D12Device(renderer_)) {
        last_error_ = "wgpu-d3d12 renderer did not expose an ID3D12Device";
        spdlog::error("[WgpuD3D12] {}", last_error_);
        shutdown();
        return false;
    }
    last_error_.clear();
    spdlog::info(
        "[WgpuD3D12] initialized adapter='{}' backend='{}' device_type='{}' "
        "nv12={} p010={} rgba16f={}",
        renderer_info_.adapter_description,
        renderer_info_.backend,
        renderer_info_.device_type,
        renderer_info_.supports_nv12 != 0,
        renderer_info_.supports_p010 != 0,
        renderer_info_.supports_rgba16_float != 0);
    return true;
#else
    last_error_ =
        "wgpu-d3d12 Rust FFI is not linked; D3D11 fallback is disabled";
    spdlog::error("[WgpuD3D12] {}", last_error_);
    return false;
#endif
}

void WgpuD3D12PresentationBackend::shutdown() {
    if (renderer_) {
        VPWgpuD3D12RendererDestroy(renderer_);
        renderer_ = nullptr;
    }
    renderer_info_ = VPWgpuD3D12RendererInfo{};
    headless_ = false;
}

void* WgpuD3D12PresentationBackend::native_render_device() const {
#if VOIDPLAYER_WGPU_RUST_LINKED
    return renderer_ ? VPWgpuD3D12RendererD3D12Device(renderer_) : nullptr;
#else
    return nullptr;
#endif
}

PresentationBackendDiagnostics WgpuD3D12PresentationBackend::diagnostics() const {
    PresentationBackendDiagnostics diagnostics;
    diagnostics.backend = "wgpu-d3d12";
    diagnostics.fallback_reason = last_error_.empty() ? "none" : last_error_;
    diagnostics.target_format = "R16G16B16A16_FLOAT";
    diagnostics.render_target_format = "wgpu-d3d12";
    diagnostics.render_color_space = "unknown";
    diagnostics.headless = headless_;
    diagnostics.adapter_description = renderer_info_.adapter_description;
    diagnostics.driver_type = renderer_info_.driver_type;
    diagnostics.adapter_vendor_id = static_cast<int32_t>(renderer_info_.vendor_id);
    diagnostics.adapter_device_id = static_cast<int32_t>(renderer_info_.device_id);
    return diagnostics;
}

bool WgpuD3D12PresentationBackend::draw_frame(
    const RendererDrawSnapshot& snapshot,
    const PresentationBackendDrawHooks& hooks) {
    (void)snapshot;
    (void)hooks;
    last_error_ =
        "wgpu-d3d12 presentation backend draw path is not implemented yet";
    return false;
}

} // namespace vr
