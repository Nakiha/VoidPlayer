#include "macos/decode/videotoolbox_provider.h"

#include "media/ffmpeg_lifetime.h"

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

    switch (codec->id) {
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_VP9:
            break;
        default:
            spdlog::info("[VideoToolbox] Codec {} is not enabled for VoidPlayer hwdownload path yet",
                         codec->name ? codec->name : "unknown");
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
    if (params.backend != RenderBackendKind::Metal &&
        params.backend != RenderBackendKind::WgpuMetal &&
        params.device_mode != DecodeDeviceMode::FfmpegOwnedHwDownloadDevice) {
        spdlog::info("[VideoToolbox] Renderer-owned CVPixelBuffer output requires Metal-compatible backend");
        return result;
    }

    AvBufferRefOwner hw_dev_ref;
    const int ret = av_hwdevice_ctx_create(
        hw_dev_ref.put(),
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

    result.success = true;
    result.hw_device_ctx = hw_dev_ref.release();
    result.hw_pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
    result.type = HwDecodeType::VideoToolbox;
    return result;
}

void VideoToolboxProvider::shutdown() {
}

void VideoToolboxProvider::flush() {
}

} // namespace vr
