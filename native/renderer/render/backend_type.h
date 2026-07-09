#pragma once

namespace vr {

enum class RenderBackendKind {
    Unknown = 0,
    Metal,
    NativeD3D11,
    NativeD3D12,
    Vulkan,
};

inline constexpr RenderBackendKind default_render_backend_kind() {
#ifdef _WIN32
    return RenderBackendKind::Unknown;
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
    case RenderBackendKind::Metal:
        return "metal";
    case RenderBackendKind::NativeD3D11:
        return "native-d3d11";
    case RenderBackendKind::NativeD3D12:
        return "native-d3d12";
    case RenderBackendKind::Vulkan:
        return "vulkan";
    }
    return "unknown";
}

} // namespace vr
