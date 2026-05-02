#pragma once
#include "video_renderer/buffer/bidi_ring_buffer.h"
#include "video_renderer/decode/hw/hw_decode_provider.h"
#include <cstdint>
#include <mutex>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace vr {

class FrameConverter {
public:
    FrameConverter();
    ~FrameConverter();

    bool init_software(int src_width, int src_height, AVPixelFormat src_format);
    bool init_hardware(void* d3d_device, void* d3d_context,
                       int src_width, int src_height,
                       HwDecodeType hw_type = HwDecodeType::None,
                       bool download_to_cpu = false,
                       std::recursive_mutex* device_mutex = nullptr);

    TextureFrame convert(AVFrame* frame);
    std::optional<TextureFrame> snapshot_hardware_frame(AVFrame* frame);

    bool is_hardware() const { return is_hw_; }
    bool downloads_hardware_to_cpu() const { return is_hw_ && download_hw_to_cpu_; }

private:
    int width_ = 0;
    int height_ = 0;
    bool is_hw_ = false;
    bool download_hw_to_cpu_ = false;
    HwDecodeType hw_type_ = HwDecodeType::None;
    SwsContext* sws_ctx_ = nullptr;
    AVPixelFormat src_format_ = AV_PIX_FMT_NONE;
    AVPixelFormat downloaded_format_ = AV_PIX_FMT_NONE;
    int sws_src_width_ = 0;
    int sws_src_height_ = 0;
    AVPixelFormat sws_src_format_ = AV_PIX_FMT_NONE;
    VideoColorInfo sws_color_;
    void* d3d_device_ = nullptr;
    void* d3d_context_ = nullptr;
    std::recursive_mutex* device_mutex_ = nullptr;

    void reset_sws_context();
    bool ensure_sws_context(int src_width, int src_height, AVPixelFormat src_format,
                            const VideoColorInfo& color);
};

} // namespace vr
