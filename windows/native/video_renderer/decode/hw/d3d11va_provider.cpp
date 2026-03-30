#include "video_renderer/decode/hw/d3d11va_provider.h"
#include <spdlog/spdlog.h>

// D3D11 headers
#include <d3d11.h>

// FFmpeg D3D11VA hwcontext
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

namespace vr {

// Lock/unlock callbacks for FFmpeg's AVD3D11VADeviceContext.
// D3D11 devices are not thread-safe; these serialize access between
// the decode thread (FFmpeg internals) and the render thread.
static void d3d11va_lock(void* lock_ctx) {
    auto* mtx = static_cast<std::recursive_mutex*>(lock_ctx);
    mtx->lock();
}

static void d3d11va_unlock(void* lock_ctx) {
    auto* mtx = static_cast<std::recursive_mutex*>(lock_ctx);
    mtx->unlock();
}

D3D11VAProvider::~D3D11VAProvider() {
    shutdown();
}

bool D3D11VAProvider::probe(const AVCodec* codec) const {
    if (!codec) return false;

    AVPixelFormat found_vld = AV_PIX_FMT_NONE;
    AVPixelFormat found_d3d11 = AV_PIX_FMT_NONE;

    for (int i = 0; ; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) break;

        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            continue;

        if (config->pix_fmt == AV_PIX_FMT_D3D11VA_VLD) {
            spdlog::debug("[D3D11VA] Found D3D11VA_VLD hw config entry {} for codec {}",
                         i, codec->name);
            found_vld = config->pix_fmt;
        } else if (config->pix_fmt == AV_PIX_FMT_D3D11) {
            spdlog::debug("[D3D11VA] Found D3D11 hw config entry {} for codec {}",
                         i, codec->name);
            found_d3d11 = config->pix_fmt;
        }
    }

    // Prefer D3D11VA_VLD over D3D11. The VLD path is the well-tested legacy
    // code path that works with hw_device_ctx alone. The newer D3D11 pix_fmt
    // requires explicit hw_frames_ctx setup which we don't provide.
    if (found_vld != AV_PIX_FMT_NONE) {
        probed_pix_fmt_ = found_vld;
        return true;
    }
    if (found_d3d11 != AV_PIX_FMT_NONE) {
        probed_pix_fmt_ = found_d3d11;
        return true;
    }
    return false;
}

HwDecodeInitResult D3D11VAProvider::init(void* native_device, int width, int height,
                                          std::recursive_mutex* external_mutex) {
    HwDecodeInitResult result;

    auto* d3d_device = static_cast<ID3D11Device*>(native_device);
    if (!d3d_device) {
        spdlog::error("[D3D11VA] No D3D11 device provided");
        return result;
    }

    // 1. Allocate FFmpeg hardware device context
    AVBufferRef* hw_dev_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_dev_ref) {
        spdlog::error("[D3D11VA] Failed to allocate hw device context");
        return result;
    }

    auto* dev_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_dev_ref->data);
    auto* d3d11_ctx = reinterpret_cast<AVD3D11VADeviceContext*>(dev_ctx->hwctx);

    // 2. Populate device context with existing D3D11 device
    d3d_device->AddRef();
    d3d11_ctx->device = d3d_device;

    ID3D11DeviceContext* immediate_ctx = nullptr;
    d3d_device->GetImmediateContext(&immediate_ctx);
    d3d11_ctx->device_context = immediate_ctx;  // Already AddRef'd by GetImmediateContext

    // 3. Bind flags: allow creating SRVs directly on decoder textures (Win8+)
    // This avoids needing a texture copy from decoder output to shader-visible texture.
    d3d11_ctx->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    d3d11_ctx->MiscFlags = 0;

    // 4. Thread safety: recursive mutex for D3D11 device access serialization
    // FFmpeg may call lock() multiple times before unlock() in some code paths,
    // hence recursive_mutex rather than mutex.
    // Use the external mutex if provided (shared across all tracks + render thread),
    // otherwise create one (for standalone provider usage).
    if (external_mutex) {
        device_mutex_.reset();  // Don't own, use external
        d3d11_ctx->lock = d3d11va_lock;
        d3d11_ctx->unlock = d3d11va_unlock;
        d3d11_ctx->lock_ctx = external_mutex;
    } else {
        device_mutex_ = std::make_unique<std::recursive_mutex>();
        d3d11_ctx->lock = d3d11va_lock;
        d3d11_ctx->unlock = d3d11va_unlock;
        d3d11_ctx->lock_ctx = device_mutex_.get();
    }

    // 5. Initialize the hardware device context
    int ret = av_hwdevice_ctx_init(hw_dev_ref);
    if (ret < 0) {
        spdlog::error("[D3D11VA] av_hwdevice_ctx_init failed: {}", ret);
        av_buffer_unref(&hw_dev_ref);
        device_mutex_.reset();
        return result;
    }

    spdlog::info("[D3D11VA] Device context initialized ({}x{}, BindFlags=DECODER|SHADER_RESOURCE)",
                 width, height);

    result.success = true;
    result.hw_device_ctx = hw_dev_ref;
    // Use the actual probed pixel format (D3D11VA_VLD or D3D11 depending on FFmpeg version)
    result.hw_pix_fmt = (probed_pix_fmt_ != AV_PIX_FMT_NONE) ? probed_pix_fmt_ : AV_PIX_FMT_D3D11VA_VLD;
    result.type = HwDecodeType::D3D11VA;
    return result;
}

void D3D11VAProvider::shutdown() {
    // device_mutex_ is kept alive until provider destruction to ensure
    // FFmpeg's teardown lock/unlock callbacks remain valid.
    // AVBufferRef (hw_device_ctx) ownership was transferred to caller.
}

} // namespace vr
