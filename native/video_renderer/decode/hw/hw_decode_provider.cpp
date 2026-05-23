#include "video_renderer/decode/hw/hw_decode_provider.h"
#ifdef _WIN32
#include "video_renderer/decode/hw/d3d11va_provider.h"
#endif
#ifdef __APPLE__
#include "video_renderer/decode/hw/videotoolbox_provider.h"
#endif
#include <spdlog/spdlog.h>
#include <vector>
#include <memory>

namespace vr {

HwDecodeInitResult try_hw_decode_providers(
    const AVCodec* codec,
    const HwDecodeInitParams& params)
{
    if (!codec) {
        spdlog::debug("[HWDecode] Skipping: no codec");
        return {};
    }

    // Priority-ordered list of hardware decode providers.
    // To add a new backend, instantiate it here.
    std::vector<std::unique_ptr<HwDecodeProvider>> providers;
#ifdef _WIN32
    providers.push_back(std::make_unique<D3D11VAProvider>());
#endif
#ifdef __APPLE__
    if (params.backend == RenderBackendType::Metal ||
        params.device_mode == DecodeDeviceMode::FfmpegOwnedHwDownloadDevice) {
        providers.push_back(std::make_unique<VideoToolboxProvider>());
    }
#endif
    // Future: providers.push_back(std::make_unique<CUDAProvider>());
    // Future: providers.push_back(std::make_unique<DXVA2Provider>());

    for (auto& provider : providers) {
        spdlog::info("[HWDecode] Probing {} for codec {}",
                     provider->name(), codec->name);

        if (!provider->probe(codec)) {
            spdlog::info("[HWDecode] {} declined (codec not supported)", provider->name());
            continue;
        }

        auto result = provider->init(params);
        if (result.success) {
            spdlog::info("[HWDecode] {} initialized successfully", provider->name());
            result.provider = std::move(provider);  // Transfer ownership — provider must outlive hw_device_ctx
            return result;
        }

        spdlog::warn("[HWDecode] {} init failed, trying next provider", provider->name());
    }

    spdlog::info("[HWDecode] No hardware decoder available, will use software decode");
    return {};
}

const char* hw_decode_type_name(HwDecodeType type) {
    switch (type) {
    case HwDecodeType::None:
        return "none";
    case HwDecodeType::D3D11VA:
        return "D3D11VA";
    case HwDecodeType::CUDA:
        return "CUDA";
    case HwDecodeType::DXVA2:
        return "DXVA2";
    case HwDecodeType::Vulkan:
        return "Vulkan";
    case HwDecodeType::VideoToolbox:
        return "VideoToolbox";
    }
    return "unknown";
}

} // namespace vr
