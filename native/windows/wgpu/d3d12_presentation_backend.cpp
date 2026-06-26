#include "windows/wgpu/d3d12_presentation_backend.h"

#include <spdlog/spdlog.h>

namespace vr {

WgpuD3D12PresentationBackend::~WgpuD3D12PresentationBackend() {
    shutdown();
}

bool WgpuD3D12PresentationBackend::initialize(
    const PresentationBackendConfig& config) {
    headless_ = config.headless;
    last_error_ =
        "wgpu-d3d12 presentation backend is not implemented; D3D11 fallback is disabled";
    spdlog::error("[WgpuD3D12] {}", last_error_);
    return false;
}

void WgpuD3D12PresentationBackend::shutdown() {
    headless_ = false;
}

PresentationBackendDiagnostics WgpuD3D12PresentationBackend::diagnostics() const {
    PresentationBackendDiagnostics diagnostics;
    diagnostics.backend = "wgpu-d3d12";
    diagnostics.fallback_reason = last_error_;
    diagnostics.target_format = "unknown";
    diagnostics.render_target_format = "unknown";
    diagnostics.render_color_space = "unknown";
    diagnostics.headless = headless_;
    return diagnostics;
}

bool WgpuD3D12PresentationBackend::draw_frame(
    const RendererDrawSnapshot& snapshot,
    const PresentationBackendDrawHooks& hooks) {
    (void)snapshot;
    (void)hooks;
    last_error_ =
        "wgpu-d3d12 presentation backend draw requested before implementation";
    return false;
}

} // namespace vr
