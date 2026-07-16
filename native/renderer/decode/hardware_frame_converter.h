#pragma once

#include "renderer/buffer/bidi_ring_buffer.h"
#include "renderer/decode/hw/hw_decode_provider.h"
#ifdef _WIN32
#include "windows/decode/d3d11_frame_snapshot.h"
#endif

#include <memory>
#include <mutex>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace vr {

struct HardwareSnapshotPoolStats {
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

class HardwareFrameConverter {
public:
    bool init(void* native_device, void* native_context,
              int src_width, int src_height,
              HwDecodeType hw_type,
              bool download_to_cpu,
              std::recursive_mutex* device_mutex);

    bool downloads_to_cpu() const { return download_to_cpu_; }
    HwDecodeType hw_type() const { return hw_type_; }
    bool snapshot_submits_shared_visibility() const {
        return !download_to_cpu_ && hw_type_ == HwDecodeType::D3D11VA;
    }

    std::optional<TextureFrame> convert(AVFrame* frame);
    std::optional<TextureFrame> snapshot_frame(AVFrame* frame);
    HardwareSnapshotPoolStats snapshot_pool_stats() const;

private:
    int width_ = 0;
    int height_ = 0;
    bool download_to_cpu_ = false;
    HwDecodeType hw_type_ = HwDecodeType::None;
    AVPixelFormat downloaded_format_ = AV_PIX_FMT_NONE;
    std::recursive_mutex* device_mutex_ = nullptr;
#ifdef _WIN32
    std::shared_ptr<D3D11SnapshotPool> d3d11_snapshot_pool_;
#endif
};

} // namespace vr
