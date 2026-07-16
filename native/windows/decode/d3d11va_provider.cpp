#include "windows/decode/d3d11va_provider.h"

#include "media/ffmpeg_lifetime.h"

#include <spdlog/spdlog.h>

#include <iterator>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

namespace vr {
namespace {

void d3d11va_lock(void* context) {
    static_cast<std::recursive_mutex*>(context)->lock();
}

void d3d11va_unlock(void* context) {
    static_cast<std::recursive_mutex*>(context)->unlock();
}

bool create_independent_decode_device(
    Microsoft::WRL::ComPtr<ID3D11Device>& device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>& context) {
    constexpr D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
    };
    constexpr D3D_DRIVER_TYPE driver_types[] = {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
    };

    for (const auto driver_type : driver_types) {
        const HRESULT result = D3D11CreateDevice(
            nullptr,
            driver_type,
            nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            feature_levels,
            static_cast<UINT>(std::size(feature_levels)),
            D3D11_SDK_VERSION,
            device.GetAddressOf(),
            nullptr,
            context.GetAddressOf());
        if (SUCCEEDED(result) && device && context) {
            spdlog::info(
                "[D3D11VA] Created independent decode device (driver_type={})",
                static_cast<int>(driver_type));
            return true;
        }
        device.Reset();
        context.Reset();
    }

    spdlog::error("[D3D11VA] Failed to create an independent decode device");
    return false;
}

} // namespace

D3D11VAProvider::~D3D11VAProvider() {
    shutdown();
}

bool D3D11VAProvider::probe(const AVCodec* codec) const {
    if (!codec) {
        return false;
    }

    probed_codec_id_ = codec->id;
    AVPixelFormat d3d11_format = AV_PIX_FMT_NONE;
    AVPixelFormat legacy_vld_format = AV_PIX_FMT_NONE;
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, index);
        if (!config) {
            break;
        }
        if (config->device_type != AV_HWDEVICE_TYPE_D3D11VA ||
            !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            continue;
        }
        if (config->pix_fmt == AV_PIX_FMT_D3D11) {
            d3d11_format = config->pix_fmt;
        } else if (config->pix_fmt == AV_PIX_FMT_D3D11VA_VLD) {
            legacy_vld_format = config->pix_fmt;
        }
    }

    probed_pixel_format_ = d3d11_format != AV_PIX_FMT_NONE
        ? d3d11_format
        : legacy_vld_format;
    if (probed_pixel_format_ == AV_PIX_FMT_NONE) {
        spdlog::info(
            "[D3D11VA] Codec {} exposes no compatible hardware config",
            codec->name ? codec->name : "unknown");
        return false;
    }
    return true;
}

HwDecodeInitResult D3D11VAProvider::init(const HwDecodeInitParams& params) {
    HwDecodeInitResult result;
    shutdown();

    ID3D11Device* decode_device = nullptr;
    if (params.device_mode == DecodeDeviceMode::SharedRenderDevice) {
        decode_device = static_cast<ID3D11Device*>(params.render_device);
        if (!decode_device) {
            spdlog::error("[D3D11VA] SharedRenderDevice has no D3D11 device");
            return result;
        }
        decode_device->GetImmediateContext(&device_context_);
        uses_shared_device_ = true;
        spdlog::warn(
            "[D3D11VA] Shared render device mode enabled; independent decode is the product default");
    } else {
        if (!create_independent_decode_device(owned_device_, device_context_)) {
            return result;
        }
        decode_device = owned_device_.Get();
    }

    if (!decode_device || !device_context_) {
        spdlog::error("[D3D11VA] Decode device or immediate context is missing");
        shutdown();
        return result;
    }

    AvBufferRefOwner device_ref(
        av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA));
    if (!device_ref) {
        spdlog::error("[D3D11VA] Failed to allocate FFmpeg device context");
        shutdown();
        return result;
    }

    auto* av_device_context =
        reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
    auto* d3d11_context =
        reinterpret_cast<AVD3D11VADeviceContext*>(av_device_context->hwctx);

    decode_device->AddRef();
    d3d11_context->device = decode_device;
    device_context_->AddRef();
    d3d11_context->device_context = device_context_.Get();

    const bool direct_same_device =
        params.device_mode == DecodeDeviceMode::SharedRenderDevice;
    d3d11_context->BindFlags = direct_same_device
        ? D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE
        : D3D11_BIND_DECODER;
    d3d11_context->MiscFlags = 0;

    if (params.device_mutex) {
        active_mutex_ = params.device_mutex;
    } else {
        device_mutex_ = std::make_unique<std::recursive_mutex>();
        active_mutex_ = device_mutex_.get();
    }
    d3d11_context->lock = d3d11va_lock;
    d3d11_context->unlock = d3d11va_unlock;
    d3d11_context->lock_ctx = active_mutex_;

    const int status = av_hwdevice_ctx_init(device_ref.get());
    if (status < 0) {
        spdlog::error("[D3D11VA] FFmpeg device initialization failed: {}", status);
        // FFmpeg may call the configured lock callbacks while releasing the
        // failed context, so release it before destroying the mutex.
        device_ref.reset();
        shutdown();
        return result;
    }

    result.success = true;
    result.hw_device_ctx = device_ref.release();
    result.hw_pix_fmt = probed_pixel_format_ != AV_PIX_FMT_NONE
        ? probed_pixel_format_
        : AV_PIX_FMT_D3D11;
    result.type = HwDecodeType::D3D11VA;
    spdlog::info(
        "[D3D11VA] Decode device initialized ({}x{}, mode={})",
        params.width,
        params.height,
        direct_same_device ? "shared-render-device" : "independent-device");
    return result;
}

void D3D11VAProvider::shutdown() {
    if (device_context_) {
        const auto flush_context = [this]() {
            if (!uses_shared_device_) {
                device_context_->ClearState();
            }
            device_context_->Flush();
        };
        if (active_mutex_) {
            std::lock_guard<std::recursive_mutex> lock(*active_mutex_);
            flush_context();
        } else {
            flush_context();
        }
    }
    owned_device_.Reset();
    device_context_.Reset();
    active_mutex_ = nullptr;
    device_mutex_.reset();
    uses_shared_device_ = false;
}

void D3D11VAProvider::flush() {
    if (!device_context_) {
        return;
    }
    if (active_mutex_) {
        std::lock_guard<std::recursive_mutex> lock(*active_mutex_);
        device_context_->Flush();
    } else {
        device_context_->Flush();
    }
}

} // namespace vr
