#pragma once

#include <d3d11.h>
#include <cstdint>

namespace vr {

inline uint64_t estimate_dxgi_surface_bytes(UINT width,
                                            UINT height,
                                            DXGI_FORMAT format,
                                            UINT array_size = 1) {
    if (width == 0 || height == 0 || array_size == 0) {
        return 0;
    }

    const uint64_t pixels = static_cast<uint64_t>(width) *
        static_cast<uint64_t>(height) *
        static_cast<uint64_t>(array_size);
    switch (format) {
    case DXGI_FORMAT_NV12:
        return pixels * 3 / 2;
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
        return pixels * 3;
    case DXGI_FORMAT_R8_UNORM:
        return pixels;
    case DXGI_FORMAT_R8G8_UNORM:
        return pixels * 2;
    case DXGI_FORMAT_R16_UNORM:
        return pixels * 2;
    case DXGI_FORMAT_R16G16_UNORM:
        return pixels * 4;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return pixels * 4;
    default:
        return 0;
    }
}

inline uint64_t estimate_d3d11_texture_bytes(const D3D11_TEXTURE2D_DESC& desc) {
    return estimate_dxgi_surface_bytes(
        desc.Width,
        desc.Height,
        desc.Format,
        desc.ArraySize);
}

} // namespace vr
