#pragma once

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

struct ID3D12Fence;
struct ID3D12Resource;

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
    // NV12/P010 hardware textures require even coded dimensions; display can remain odd.
    int coded_width = 0;
    int coded_height = 0;
};

enum class CpuYuvPlaneLayout : int {
    PlanarYuv420 = 0,
    SemiPlanarYuv420 = 1,
};

enum class CpuYuvSampleAlignment : int {
    Packed = 0,
    MsbAligned = 1,
};

struct CpuPlanarYuvFrameStorage {
    std::shared_ptr<void> frame_ref;
    const uint8_t* planes[3] = {};
    int strides[3] = {};
    int plane_widths[3] = {};
    int plane_heights[3] = {};
    int bytes_per_sample = 1;
    int bit_depth = 8;
    CpuYuvPlaneLayout plane_layout = CpuYuvPlaneLayout::PlanarYuv420;
    CpuYuvSampleAlignment sample_alignment = CpuYuvSampleAlignment::Packed;
};

struct D3D12TextureFrameStorage {
    ID3D12Resource* texture = nullptr;
    int subresource_index = 0;
    ID3D12Fence* fence = nullptr;
    void* fence_event = nullptr;
    uint64_t fence_value = 0;
    bool is_texture_array = false;
    bool is_p010 = false;
    int coded_width = 0;
    int coded_height = 0;
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
    D3D12TextureFrameStorage,
    MacOSCVPixelBufferFrameStorage>;

enum class FrameStorageKind {
    Empty,
    CpuRgba,
    CpuNv12,
    CpuPlanarYuv,
    D3D12Texture,
    MacOSCVPixelBuffer,
};

enum class FrameStorageClass {
    Empty,
    CpuPixels,
    HardwareTexture,
    CVPixelBuffer,
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
    if (std::holds_alternative<D3D12TextureFrameStorage>(storage)) {
        return FrameStorageKind::D3D12Texture;
    }
    if (std::holds_alternative<MacOSCVPixelBufferFrameStorage>(storage)) {
        return FrameStorageKind::MacOSCVPixelBuffer;
    }
    return FrameStorageKind::Empty;
}

inline FrameStorageClass frame_storage_class(FrameStorageKind kind) {
    switch (kind) {
    case FrameStorageKind::CpuRgba:
    case FrameStorageKind::CpuNv12:
    case FrameStorageKind::CpuPlanarYuv:
        return FrameStorageClass::CpuPixels;
    case FrameStorageKind::D3D12Texture:
        return FrameStorageClass::HardwareTexture;
    case FrameStorageKind::MacOSCVPixelBuffer:
        return FrameStorageClass::CVPixelBuffer;
    case FrameStorageKind::Empty:
    default:
        return FrameStorageClass::Empty;
    }
}

inline FrameStorageClass frame_storage_class(const FrameStorage& storage) {
    return frame_storage_class(frame_storage_kind(storage));
}

inline bool frame_storage_has_cpu_pixels(FrameStorageKind kind) {
    return frame_storage_class(kind) == FrameStorageClass::CpuPixels;
}

} // namespace vr
