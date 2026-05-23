#include "video_renderer/decode/hw/videotoolbox_provider.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/error.h>
}

namespace vr {

VideoToolboxProvider::~VideoToolboxProvider() {
    shutdown();
}

bool VideoToolboxProvider::probe(const AVCodec* codec) const {
    if (!codec) {
        return false;
    }

    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            break;
        }
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
            config->device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX) {
            return true;
        }
    }
    return false;
}

HwDecodeInitResult VideoToolboxProvider::init(const HwDecodeInitParams& params) {
    HwDecodeInitResult result;
    if (params.device_mode != DecodeDeviceMode::FfmpegOwnedHwDownloadDevice) {
        spdlog::info("[VideoToolbox] Renderer-owned CVPixelBuffer output is not wired yet; "
                     "use FfmpegOwnedHwDownloadDevice for the current shared pipeline");
        return result;
    }

    AVBufferRef* hw_dev_ref = nullptr;
    const int ret = av_hwdevice_ctx_create(
        &hw_dev_ref,
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
        nullptr,
        nullptr,
        0);
    if (ret < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, error, sizeof(error));
        spdlog::warn("[VideoToolbox] av_hwdevice_ctx_create failed: {}", error);
        return result;
    }

    hw_device_ctx_ = hw_dev_ref;
    result.success = true;
    result.hw_device_ctx = hw_dev_ref;
    result.hw_pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
    result.type = HwDecodeType::VideoToolbox;
    return result;
}

void VideoToolboxProvider::shutdown() {
    // The AVBufferRef is transferred to DecodeThread on success. If init()
    // fails before transfer, this still makes repeated tests deterministic.
    hw_device_ctx_ = nullptr;
}

void VideoToolboxProvider::flush() {
}

} // namespace vr
