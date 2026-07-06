#pragma once
#include "renderer/decode/frame_identity_types.h"
#include "renderer/frame/frame_storage.h"
#include <vector>
#include <mutex>
#include <optional>
#include <cstdint>
#include <memory>

namespace vr {

struct TextureFrame {
    int64_t pts_us = 0;
    int64_t duration_us = 0;
    int width = 0;
    int height = 0;
    bool is_ref = false;
    void* texture_handle = nullptr;
    int64_t dts_us = kNoTimestampUs;
    int32_t analysis_frame_index = kInvalidAnalysisFrameIndex;
    int32_t source_packet_index = kInvalidSourcePacketIndex;
    int32_t source_packet_size = 0;
    int64_t source_packet_pos = kUnknownSourcePacketPosition;
    int64_t source_packet_pts = kNoTimestampUs;
    int64_t source_packet_dts = kNoTimestampUs;
    FrameIdentityMode frame_identity_mode = FrameIdentityMode::Unknown;
    // Owns CPU-side video data; shared_ptr enables safe cross-thread sharing
    // and automatic cleanup when all references are gone.
    std::shared_ptr<std::vector<uint8_t>> cpu_data;
    FrameStorage storage;

    // Planar YUV metadata for software NV12/P010 upload.
    bool is_nv12 = false;               // true if frame uses Y + interleaved UV sampling
    bool is_p010 = false;               // true for CPU P010 upload
    int texture_array_index = 0;        // Texture2DArray slice index
    VideoColorInfo color;

    // Holds a reference to the underlying AVFrame/hw buffer. Prevents the
    // decoder from reusing the frame pool slot while the render thread
    // still has a TextureFrame pointing to it. Released automatically via
    // shared_ptr deleter (calls av_frame_free or av_buffer_unref).
    std::shared_ptr<void> hw_frame_ref;

    FrameStorageKind storage_kind() const { return frame_storage_kind(storage); }
    FrameStorageClass storage_class() const { return frame_storage_class(storage); }
    const CpuRgbaFrameStorage* cpu_rgba_storage() const {
        return std::get_if<CpuRgbaFrameStorage>(&storage);
    }
    const CpuNv12FrameStorage* cpu_nv12_storage() const {
        return std::get_if<CpuNv12FrameStorage>(&storage);
    }
    const CpuPlanarYuvFrameStorage* cpu_planar_yuv_storage() const {
        return std::get_if<CpuPlanarYuvFrameStorage>(&storage);
    }
    const D3D12TextureFrameStorage* d3d12_texture_storage() const {
        return std::get_if<D3D12TextureFrameStorage>(&storage);
    }
    const MacOSCVPixelBufferFrameStorage* cv_pixel_buffer_storage() const {
        return std::get_if<MacOSCVPixelBufferFrameStorage>(&storage);
    }

    const MacOSCVPixelBufferFrameStorage* macos_cv_pixel_buffer_storage() const {
        return cv_pixel_buffer_storage();
    }
};

inline uint64_t estimate_texture_frame_cpu_bytes(const TextureFrame& frame) {
    if (frame.cpu_data) {
        return static_cast<uint64_t>(frame.cpu_data->capacity());
    }
    if (const auto* storage = frame.cpu_rgba_storage()) {
        return storage->data ? static_cast<uint64_t>(storage->data->capacity()) : 0;
    }
    if (const auto* storage = frame.cpu_nv12_storage()) {
        return storage->data ? static_cast<uint64_t>(storage->data->capacity()) : 0;
    }
    if (const auto* storage = frame.cpu_planar_yuv_storage()) {
        uint64_t bytes = 0;
        for (int i = 0; i < 3; ++i) {
            if (!storage->planes[i] || storage->strides[i] == 0 ||
                storage->plane_heights[i] <= 0) {
                continue;
            }
            const uint64_t stride = static_cast<uint64_t>(
                storage->strides[i] < 0 ? -storage->strides[i] : storage->strides[i]);
            bytes += stride * static_cast<uint64_t>(storage->plane_heights[i]);
        }
        return bytes;
    }
    return 0;
}

class BidiRingBuffer {
public:
    explicit BidiRingBuffer(size_t forward_depth = 4, size_t backward_depth = 2);

    // Write side (Decode thread)
    struct PushTiming {
        uint64_t lock_us = 0;
        uint64_t assign_us = 0;
        uint64_t advance_us = 0;
        uint64_t overwritten_cpu_bytes = 0;
    };

    bool push(TextureFrame frame, PushTiming* timing = nullptr);

    // Read side (Render thread)
    std::optional<TextureFrame> peek(int offset = 0) const;
    bool advance();    // read_idx++
    bool retreat();    // read_idx--
    bool can_advance() const;
    bool can_retreat() const;

    // State
    size_t capacity() const { return capacity_; }
    /// Max push-able frames: reserves backward_depth slots behind read_idx
    /// so retreat never lands on a push-overwritten slot.
    size_t max_count() const { return capacity_ - backward_depth_; }
    size_t forward_count() const;
    size_t backward_count() const;
    size_t available_retreat_count() const;
    size_t total_count() const;
    uint64_t estimated_cpu_bytes() const;
    bool empty() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<TextureFrame> ring_;
    size_t capacity_;
    size_t backward_depth_;
    size_t write_idx_ = 0;
    size_t read_idx_ = 0;
    size_t count_ = 0;
    size_t retreated_ = 0;       // How many times we've retreated past the last advance position
    size_t total_advanced_ = 0;  // Total advances since last clear (limits valid retreat range)
};

} // namespace vr
