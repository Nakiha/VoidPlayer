#pragma once

namespace vr {

enum class RenderBackendKind {
    Unknown = 0,
    D3D11,
    Metal,
    WgpuMetal,
    WgpuD3D12,
    Vulkan,
};

inline const char* render_backend_kind_name(RenderBackendKind kind) {
    switch (kind) {
    case RenderBackendKind::Unknown:
        return "unknown";
    case RenderBackendKind::D3D11:
        return "d3d11";
    case RenderBackendKind::Metal:
        return "metal";
    case RenderBackendKind::WgpuMetal:
        return "wgpu-metal";
    case RenderBackendKind::WgpuD3D12:
        return "wgpu-d3d12";
    case RenderBackendKind::Vulkan:
        return "vulkan";
    }
    return "unknown";
}

} // namespace vr
