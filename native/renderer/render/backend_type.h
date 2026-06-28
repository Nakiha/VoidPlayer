#pragma once

namespace vr {

enum class RenderBackendKind {
    Unknown = 0,
    Metal,
    WgpuMetal,
    WgpuD3D12,
    Vulkan,
};

inline constexpr RenderBackendKind default_render_backend_kind() {
#ifdef _WIN32
    return RenderBackendKind::WgpuD3D12;
#elif defined(__APPLE__)
    return RenderBackendKind::WgpuMetal;
#else
    return RenderBackendKind::Unknown;
#endif
}

inline const char* render_backend_kind_name(RenderBackendKind kind) {
    switch (kind) {
    case RenderBackendKind::Unknown:
        return "unknown";
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
