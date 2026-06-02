#pragma once

#include "renderer/buffer/bidi_ring_buffer.h"
#include "renderer/decode/d3d11_frame_snapshot.h"
#include "renderer/decode/hw/hw_decode_provider.h"

#include <memory>
#include <mutex>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace vr {

class HardwareFrameConverter {
public:
    bool init(void* d3d_device, void* d3d_context,
              int src_width, int src_height,
              HwDecodeType hw_type,
              bool download_to_cpu,
              std::recursive_mutex* device_mutex);

    bool downloads_to_cpu() const { return download_to_cpu_; }
    HwDecodeType hw_type() const { return hw_type_; }

    std::optional<TextureFrame> convert(AVFrame* frame);
    std::optional<TextureFrame> snapshot_frame(AVFrame* frame);
    D3D11SnapshotPoolStats snapshot_pool_stats() const;

private:
    int width_ = 0;
    int height_ = 0;
    bool download_to_cpu_ = false;
    HwDecodeType hw_type_ = HwDecodeType::None;
    AVPixelFormat downloaded_format_ = AV_PIX_FMT_NONE;
    std::recursive_mutex* device_mutex_ = nullptr;
    std::shared_ptr<D3D11SnapshotPool> d3d11_snapshot_pool_;
};

} // namespace vr
