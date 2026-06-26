#include "windows/decode/d3d12va_provider.h"

#include "media/ffmpeg_lifetime.h"

#include <spdlog/spdlog.h>

#include <d3d12.h>
#include <sstream>
#include <string>

extern "C" {
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/pixdesc.h>
}

namespace vr {
namespace {

void d3d12va_lock(void* lock_ctx) {
    auto* mtx = static_cast<std::recursive_mutex*>(lock_ctx);
    mtx->lock();
}

void d3d12va_unlock(void* lock_ctx) {
    auto* mtx = static_cast<std::recursive_mutex*>(lock_ctx);
    mtx->unlock();
}

bool codec_has_d3d12va_config(const AVCodec* codec, AVPixelFormat& pix_fmt) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            break;
        }
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            continue;
        }
        if (config->device_type == AV_HWDEVICE_TYPE_D3D12VA &&
            config->pix_fmt == AV_PIX_FMT_D3D12) {
            pix_fmt = config->pix_fmt;
            return true;
        }
    }
    return false;
}

std::string describe_hw_configs(const AVCodec* codec) {
    if (!codec) {
        return "none";
    }
    std::ostringstream out;
    bool any = false;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            break;
        }
        if (any) {
            out << "; ";
        }
        any = true;
        const char* device = av_hwdevice_get_type_name(config->device_type);
        const char* pix_fmt = av_get_pix_fmt_name(config->pix_fmt);
        out << "#" << i << "{device=" << (device ? device : "unknown")
            << ", pix_fmt=" << (pix_fmt ? pix_fmt : "unknown")
            << ", methods=" << config->methods << "}";
    }
    return any ? out.str() : "none";
}

} // namespace

D3D12VAProvider::~D3D12VAProvider() {
    shutdown();
}

bool D3D12VAProvider::probe(const AVCodec* codec) const {
    if (!codec) {
        return false;
    }

    probed_codec_id_ = codec->id;
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    if (!codec_has_d3d12va_config(codec, pix_fmt)) {
        spdlog::info("[D3D12VA] No compatible D3D12VA hw config for codec {}; configs={}",
                     codec->name,
                     describe_hw_configs(codec));
        return false;
    }

    probed_pix_fmt_ = pix_fmt;
    spdlog::debug("[D3D12VA] Found D3D12VA hw config for codec {}", codec->name);
    return true;
}

HwDecodeInitResult D3D12VAProvider::init(const HwDecodeInitParams& params) {
    HwDecodeInitResult result;

    AvBufferRefOwner hw_dev_ref;
    if (params.device_mode == DecodeDeviceMode::SharedRenderDevice) {
        auto* d3d12_device = static_cast<ID3D12Device*>(params.render_device);
        if (!d3d12_device) {
            spdlog::error("[D3D12VA] SharedRenderDevice requested without ID3D12Device");
            return result;
        }

        hw_dev_ref.reset(av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA));
        if (!hw_dev_ref) {
            spdlog::error("[D3D12VA] Failed to allocate shared hw device context");
            return result;
        }

        auto* dev_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_dev_ref->data);
        auto* d3d12_ctx = reinterpret_cast<AVD3D12VADeviceContext*>(dev_ctx->hwctx);
        d3d12_device->AddRef();
        d3d12_ctx->device = d3d12_device;

        if (params.device_mutex) {
            active_mutex_ = params.device_mutex;
        } else {
            device_mutex_ = std::make_unique<std::recursive_mutex>();
            active_mutex_ = device_mutex_.get();
        }
        d3d12_ctx->lock = d3d12va_lock;
        d3d12_ctx->unlock = d3d12va_unlock;
        d3d12_ctx->lock_ctx = active_mutex_;

        const int ret = av_hwdevice_ctx_init(hw_dev_ref.get());
        if (ret < 0) {
            spdlog::error("[D3D12VA] av_hwdevice_ctx_init failed for shared device: {}", ret);
            shutdown();
            return result;
        }

        spdlog::info("[D3D12VA] Initialized from shared renderer ID3D12Device ({}x{})",
                     params.width,
                     params.height);
    } else {
        const int ret = av_hwdevice_ctx_create(
            hw_dev_ref.put(),
            AV_HWDEVICE_TYPE_D3D12VA,
            nullptr,
            nullptr,
            0);
        if (ret < 0 || !hw_dev_ref) {
            spdlog::error("[D3D12VA] av_hwdevice_ctx_create failed for {}: {}",
                          avcodec_get_name(probed_codec_id_),
                          ret);
            return result;
        }

        spdlog::info("[D3D12VA] Initialized FFmpeg-owned D3D12VA device ({}x{})",
                     params.width,
                     params.height);
    }

    result.success = true;
    result.hw_device_ctx = hw_dev_ref.release();
    result.hw_pix_fmt =
        (probed_pix_fmt_ != AV_PIX_FMT_NONE) ? probed_pix_fmt_ : AV_PIX_FMT_D3D12;
    result.type = HwDecodeType::D3D12VA;
    return result;
}

void D3D12VAProvider::shutdown() {
    active_mutex_ = nullptr;
    device_mutex_.reset();
}

void D3D12VAProvider::flush() {
    // FFmpeg owns the D3D12 decode queue. Per-frame AVD3D12VA sync metadata is
    // carried with the decoded frame and consumed by the presentation import path.
}

void D3D12VAProvider::wait_idle() {
    flush();
}

} // namespace vr
