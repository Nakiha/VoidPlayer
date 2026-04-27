#pragma once

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

struct ID3D11Texture2D;

namespace vr {

struct CpuRgbaFrameStorage {
    std::shared_ptr<std::vector<uint8_t>> data;
    int stride = 0;
};

struct D3D11Nv12FrameStorage {
    ID3D11Texture2D* texture = nullptr;
    int array_index = 0;
    std::shared_ptr<void> frame_ref;
};

struct D3D11TextureFrameStorage {
    ID3D11Texture2D* texture = nullptr;
    std::shared_ptr<void> frame_ref;
};

using FrameStorage = std::variant<
    std::monostate,
    CpuRgbaFrameStorage,
    D3D11Nv12FrameStorage,
    D3D11TextureFrameStorage>;

enum class FrameStorageKind {
    Empty,
    CpuRgba,
    D3D11Nv12,
    D3D11Texture,
};

inline FrameStorageKind frame_storage_kind(const FrameStorage& storage) {
    if (std::holds_alternative<CpuRgbaFrameStorage>(storage)) {
        return FrameStorageKind::CpuRgba;
    }
    if (std::holds_alternative<D3D11Nv12FrameStorage>(storage)) {
        return FrameStorageKind::D3D11Nv12;
    }
    if (std::holds_alternative<D3D11TextureFrameStorage>(storage)) {
        return FrameStorageKind::D3D11Texture;
    }
    return FrameStorageKind::Empty;
}

} // namespace vr
