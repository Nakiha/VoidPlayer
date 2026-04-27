#include "video_renderer/decode/hw/d3d11va_provider.h"
#include <spdlog/spdlog.h>

// D3D11 headers
#include <d3d11.h>
#include <dxgi.h>

// FFmpeg D3D11VA hwcontext
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

namespace vr {

namespace {
bool create_independent_decode_device(Microsoft::WRL::ComPtr<ID3D11Device>& device,
                                      Microsoft::WRL::ComPtr<ID3D11DeviceContext>& context) {
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    UINT create_flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_DRIVER_TYPE driver_types[] = {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
    };

    HRESULT hr = E_FAIL;
    for (auto dt : driver_types) {
        hr = D3D11CreateDevice(
            nullptr, dt, nullptr, create_flags,
            feature_levels, ARRAYSIZE(feature_levels),
            D3D11_SDK_VERSION,
            device.GetAddressOf(),
            nullptr, context.GetAddressOf());
        if (SUCCEEDED(hr)) break;

        device.Reset();
        context.Reset();
    }

    if (SUCCEEDED(hr) && device && context) {
        spdlog::info("[D3D11VA] Created independent D3D11 decode device");
        return true;
    }

    spdlog::error("[D3D11VA] Failed to create independent D3D11 decode device");
    device.Reset();
    context.Reset();
    return false;
}
}  // namespace

// Lock/unlock callbacks for FFmpeg's AVD3D11VADeviceContext.
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

    probed_codec_id_ = codec->id;
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

    if (found_d3d11 != AV_PIX_FMT_NONE) {
        probed_pix_fmt_ = found_d3d11;
        return true;
    }
    if (found_vld != AV_PIX_FMT_NONE) {
        probed_pix_fmt_ = found_vld;
        return true;
    }
    return false;
}

HwDecodeInitResult D3D11VAProvider::init(const HwDecodeInitParams& params) {
    HwDecodeInitResult result;
    void* native_device = params.render_device;
    const int width = params.width;
    const int height = params.height;
    std::recursive_mutex* device_mutex = params.device_mutex;

    // AV1/VP9 use hardware decode + hwdownload before renderer upload. Let
    // FFmpeg create the D3D11VA device/context exactly like the CLI hwaccel
    // path; manually populated D3D11VA contexts can produce black transfer
    // frames on some drivers even though decode succeeds.
    const bool ffmpeg_owned_hwdownload_device =
        (probed_codec_id_ == AV_CODEC_ID_AV1 || probed_codec_id_ == AV_CODEC_ID_VP9) &&
        !native_device;
    if (ffmpeg_owned_hwdownload_device) {
        AVBufferRef* hw_dev_ref = nullptr;
        int ret = av_hwdevice_ctx_create(&hw_dev_ref, AV_HWDEVICE_TYPE_D3D11VA,
                                         nullptr, nullptr, 0);
        if (ret < 0 || !hw_dev_ref) {
            spdlog::error("[D3D11VA] av_hwdevice_ctx_create failed for {}: {}",
                          avcodec_get_name(probed_codec_id_), ret);
            return result;
        }

        auto* dev_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_dev_ref->data);
        auto* d3d11_ctx = reinterpret_cast<AVD3D11VADeviceContext*>(dev_ctx->hwctx);
        own_device_ = d3d11_ctx->device;
        d3d_context_ = d3d11_ctx->device_context;
        uses_shared_device_ = true;
        active_mutex_ = nullptr;

        spdlog::info("[D3D11VA] Device context initialized by FFmpeg for {} hwdownload ({}x{})",
                     avcodec_get_name(probed_codec_id_), width, height);

        result.success = true;
        result.hw_device_ctx = hw_dev_ref;
        result.hw_pix_fmt = (probed_pix_fmt_ != AV_PIX_FMT_NONE) ? probed_pix_fmt_ : AV_PIX_FMT_D3D11;
        result.type = HwDecodeType::D3D11VA;
        return result;
    }

    // If no external device provided, create our own independent D3D11 device.
    // This is critical for stability — sharing the render device's immediate context
    // between the decode and render threads causes D3D11VA internal state corruption
    // during seek operations. Professional players (mpv, VLC, PotPlayer) use
    // independent decode devices + DXGI shared resources for cross-device texture access.
    ID3D11Device* d3d_device = nullptr;

    if (native_device) {
        d3d_device = static_cast<ID3D11Device*>(native_device);
        d3d_device->GetImmediateContext(&d3d_context_);
        uses_shared_device_ = false;
    } else {
        if (!create_independent_decode_device(own_device_, d3d_context_)) {
            return result;
        }
        d3d_device = own_device_.Get();
        uses_shared_device_ = false;
    }

    if (!d3d_device) {
        spdlog::error("[D3D11VA] No D3D11 device available");
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

    // 2. Populate device context with D3D11 device and context.
    // Both must be AddRef'd: FFmpeg's d3d11va_device_uninit() calls Release()
    // on both device and device_context. Without matching AddRef, the context
    // would be freed prematurely, causing use-after-free in our shutdown().
    d3d_device->AddRef();
    d3d11_ctx->device = d3d_device;
    d3d_context_->AddRef();
    d3d11_ctx->device_context = d3d_context_.Get();

    // 3. Bind flags: direct renderer sampling needs shared shader-resource
    // decode surfaces. AV1/VP9 currently use hwdownload before upload; on some
    // drivers D3D11VA surfaces created with SHADER_RESOURCE|SHARED decode
    // successfully but transfer out as black frames, so keep them decoder-only.
    const bool decoder_only_surfaces =
        probed_codec_id_ == AV_CODEC_ID_AV1 || probed_codec_id_ == AV_CODEC_ID_VP9;
    if (decoder_only_surfaces) {
        d3d11_ctx->BindFlags = D3D11_BIND_DECODER;
        d3d11_ctx->MiscFlags = 0;
    } else {
        d3d11_ctx->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
        d3d11_ctx->MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    }

    // 4. Thread safety: recursive mutex for D3D11 device access serialization
    if (device_mutex) {
        device_mutex_.reset();  // Don't own, use external
        active_mutex_ = device_mutex;
        d3d11_ctx->lock = d3d11va_lock;
        d3d11_ctx->unlock = d3d11va_unlock;
        d3d11_ctx->lock_ctx = device_mutex;
    } else {
        device_mutex_ = std::make_unique<std::recursive_mutex>();
        active_mutex_ = device_mutex_.get();
        d3d11_ctx->lock = d3d11va_lock;
        d3d11_ctx->unlock = d3d11va_unlock;
        d3d11_ctx->lock_ctx = active_mutex_;
    }

    // 5. Initialize the hardware device context
    int ret = av_hwdevice_ctx_init(hw_dev_ref);
    if (ret < 0) {
        spdlog::error("[D3D11VA] av_hwdevice_ctx_init failed: {}", ret);
        av_buffer_unref(&hw_dev_ref);
        device_mutex_.reset();
        active_mutex_ = nullptr;
        uses_shared_device_ = false;
        own_device_.Reset();
        d3d_context_.Reset();
        return result;
    }

    spdlog::info("[D3D11VA] Device context initialized ({}x{}, BindFlags={}, MiscFlags={:#x})",
                 width,
                 height,
                 decoder_only_surfaces ? "DECODER" : "DECODER|SHADER_RESOURCE",
                 static_cast<unsigned int>(d3d11_ctx->MiscFlags));

    result.success = true;
    result.hw_device_ctx = hw_dev_ref;
    result.hw_pix_fmt = (probed_pix_fmt_ != AV_PIX_FMT_NONE) ? probed_pix_fmt_ : AV_PIX_FMT_D3D11VA_VLD;
    result.type = HwDecodeType::D3D11VA;
    return result;
}

void D3D11VAProvider::shutdown() {
    // device_mutex_ is kept alive until provider destruction to ensure
    // FFmpeg's teardown lock/unlock callbacks remain valid.
    // AVBufferRef (hw_device_ctx) ownership was transferred to caller.
    if (d3d_context_) {
        if (active_mutex_) {
            std::unique_lock<std::recursive_mutex> lock(*active_mutex_);
            if (!uses_shared_device_) {
                d3d_context_->ClearState();
            }
            d3d_context_->Flush();
        } else {
            if (!uses_shared_device_) {
                d3d_context_->ClearState();
            }
            d3d_context_->Flush();
        }
    }
    own_device_.Reset();
    d3d_context_.Reset();
    active_mutex_ = nullptr;
    uses_shared_device_ = false;
}

void D3D11VAProvider::flush() {
    if (d3d_context_) {
        if (active_mutex_) {
            std::unique_lock<std::recursive_mutex> lock(*active_mutex_);
            d3d_context_->Flush();
        } else {
            d3d_context_->Flush();
        }
    }
}

} // namespace vr
