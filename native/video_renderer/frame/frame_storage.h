#pragma once

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

struct ID3D11Texture2D;

namespace vr {

enum VideoColorRange : int {
    VIDEO_COLOR_RANGE_UNKNOWN = 0,
    VIDEO_COLOR_RANGE_LIMITED = 1,
    VIDEO_COLOR_RANGE_FULL = 2,
};

enum VideoColorMatrix : int {
    VIDEO_COLOR_MATRIX_UNKNOWN = 0,
    VIDEO_COLOR_MATRIX_BT601 = 1,
    VIDEO_COLOR_MATRIX_BT709 = 2,
    VIDEO_COLOR_MATRIX_BT2020_NCL = 3,
};

enum VideoColorTransfer : int {
    VIDEO_COLOR_TRANSFER_UNKNOWN = 0,
    VIDEO_COLOR_TRANSFER_SDR = 1,
    VIDEO_COLOR_TRANSFER_PQ = 2,
    VIDEO_COLOR_TRANSFER_HLG = 3,
};

enum VideoColorPrimaries : int {
    VIDEO_COLOR_PRIMARIES_UNKNOWN = 0,
    VIDEO_COLOR_PRIMARIES_BT601 = 1,
    VIDEO_COLOR_PRIMARIES_BT709 = 2,
    VIDEO_COLOR_PRIMARIES_BT2020 = 3,
};

struct VideoColorInfo {
    int range = VIDEO_COLOR_RANGE_UNKNOWN;
    int matrix = VIDEO_COLOR_MATRIX_UNKNOWN;
    int transfer = VIDEO_COLOR_TRANSFER_UNKNOWN;
    int primaries = VIDEO_COLOR_PRIMARIES_UNKNOWN;
};

struct CpuRgbaFrameStorage {
    std::shared_ptr<std::vector<uint8_t>> data;
    int stride = 0;
};

struct CpuNv12FrameStorage {
    std::shared_ptr<std::vector<uint8_t>> data;
    int y_stride = 0;
    int uv_stride = 0;
    bool is_p010 = false;
    // NV12/P010 D3D textures require even coded dimensions; display can remain odd.
    int coded_width = 0;
    int coded_height = 0;
};

struct CpuPlanarYuvFrameStorage {
    std::shared_ptr<void> frame_ref;
    const uint8_t* planes[3] = {};
    int strides[3] = {};
    int plane_widths[3] = {};
    int plane_heights[3] = {};
    int bytes_per_sample = 1;
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

struct MacOSCVPixelBufferFrameStorage {
    void* pixel_buffer = nullptr;
    uint32_t pixel_format = 0;
    int plane_count = 0;
    bool is_p010 = false;
    int coded_width = 0;
    int coded_height = 0;
    std::shared_ptr<void> frame_ref;
};

using FrameStorage = std::variant<
    std::monostate,
    CpuRgbaFrameStorage,
    CpuNv12FrameStorage,
    CpuPlanarYuvFrameStorage,
    D3D11Nv12FrameStorage,
    D3D11TextureFrameStorage,
    MacOSCVPixelBufferFrameStorage>;

enum class FrameStorageKind {
    Empty,
    CpuRgba,
    CpuNv12,
    CpuPlanarYuv,
    D3D11Nv12,
    D3D11Texture,
    MacOSCVPixelBuffer,
};

inline FrameStorageKind frame_storage_kind(const FrameStorage& storage) {
    if (std::holds_alternative<CpuRgbaFrameStorage>(storage)) {
        return FrameStorageKind::CpuRgba;
    }
    if (std::holds_alternative<CpuNv12FrameStorage>(storage)) {
        return FrameStorageKind::CpuNv12;
    }
    if (std::holds_alternative<CpuPlanarYuvFrameStorage>(storage)) {
        return FrameStorageKind::CpuPlanarYuv;
    }
    if (std::holds_alternative<D3D11Nv12FrameStorage>(storage)) {
        return FrameStorageKind::D3D11Nv12;
    }
    if (std::holds_alternative<D3D11TextureFrameStorage>(storage)) {
        return FrameStorageKind::D3D11Texture;
    }
    if (std::holds_alternative<MacOSCVPixelBufferFrameStorage>(storage)) {
        return FrameStorageKind::MacOSCVPixelBuffer;
    }
    return FrameStorageKind::Empty;
}

} // namespace vr
