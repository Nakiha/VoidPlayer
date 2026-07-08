#pragma once

namespace vr {

enum class RenderBackendKind {
    Unknown = 0,
    D3D11,
    Metal,
    Vulkan,
};

inline constexpr RenderBackendKind default_render_backend_kind() {
#ifdef _WIN32
    return RenderBackendKind::D3D11;
#elif defined(__APPLE__)
    return RenderBackendKind::Metal;
#else
    return RenderBackendKind::Unknown;
#endif
}

inline const char* render_backend_kind_name(RenderBackendKind kind) {
    switch (kind) {
    case RenderBackendKind::Unknown:
        return "unknown";
    case RenderBackendKind::D3D11:
        return "d3d11";
    case RenderBackendKind::Metal:
        return "metal";
    case RenderBackendKind::Vulkan:
        return "vulkan";
    }
    return "unknown";
}

} // namespace vr
