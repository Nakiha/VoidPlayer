#pragma once
#include "renderer/buffer/bidi_ring_buffer.h"
#include "renderer/decode/hardware_frame_converter.h"
#include "renderer/decode/hw/hw_decode_provider.h"
#include <memory>
#include <mutex>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace vr {

struct DecodeStagePerfCounters;

class FrameConverter {
public:
    FrameConverter();
    ~FrameConverter();

    bool init_software(int src_width, int src_height, AVPixelFormat src_format);
    bool init_hardware(void* native_device, void* native_context,
                       int src_width, int src_height,
                       HwDecodeType hw_type = HwDecodeType::None,
                       bool download_to_cpu = false,
                       std::recursive_mutex* device_mutex = nullptr);

    std::optional<TextureFrame> convert(AVFrame* frame,
                                        DecodeStagePerfCounters* stage_perf = nullptr);
    std::optional<TextureFrame> snapshot_hardware_frame(AVFrame* frame);

    bool is_hardware() const { return hardware_converter_ != nullptr; }
    bool downloads_hardware_to_cpu() const {
        return hardware_converter_ && hardware_converter_->downloads_to_cpu();
    }
    bool hardware_snapshot_submits_shared_visibility() const {
        return hardware_converter_ &&
               hardware_converter_->snapshot_submits_shared_visibility();
    }
    HardwareSnapshotPoolStats snapshot_pool_stats() const;

private:
    int width_ = 0;
    int height_ = 0;
    AVPixelFormat src_format_ = AV_PIX_FMT_NONE;
    std::unique_ptr<HardwareFrameConverter> hardware_converter_;
};

} // namespace vr
