#pragma once

#include "video_renderer/buffer/bidi_ring_buffer.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

extern "C" {
#include <libavutil/frame.h>
}

namespace vr {

struct D3D11SnapshotPool;

struct D3D11SnapshotPoolStats {
    uint64_t estimated_bytes = 0;
    uint64_t texture_bytes = 0;
    uint64_t created_count = 0;
    uint64_t reused_count = 0;
    size_t checked_out_count = 0;
    size_t available_count = 0;
    int width = 0;
    int height = 0;
    int format = 0;
};

bool populate_d3d11_hardware_texture_frame(AVFrame* frame, TextureFrame& result);

std::optional<TextureFrame> snapshot_d3d11_hardware_frame(
    AVFrame* frame,
    const TextureFrame& metadata,
    std::recursive_mutex* device_mutex,
    std::shared_ptr<D3D11SnapshotPool>& snapshot_pool);

D3D11SnapshotPoolStats d3d11_snapshot_pool_stats(
    const std::shared_ptr<D3D11SnapshotPool>& snapshot_pool);

} // namespace vr
