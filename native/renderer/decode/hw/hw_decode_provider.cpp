#include "renderer/decode/hw/hw_decode_provider.h"
#ifdef _WIN32
#include "windows/decode/d3d12va_provider.h"
#endif
#ifdef __APPLE__
#include "macos/decode/videotoolbox_provider.h"
#endif
#include <spdlog/spdlog.h>
#include <vector>
#include <memory>

namespace vr {
namespace {

using HwDecodeProviderFactory = std::unique_ptr<HwDecodeProvider> (*)();

struct HwDecodeProviderDescriptor {
    const char* name = "";
    RenderBackendKind backend = RenderBackendKind::Unknown;
    RenderBackendKind secondary_backend = RenderBackendKind::Unknown;
    bool allow_ffmpeg_owned_hwdownload = false;
    HwDecodeProviderFactory create = nullptr;
};

#ifdef _WIN32
std::unique_ptr<HwDecodeProvider> create_d3d12va_provider() {
    return std::make_unique<D3D12VAProvider>();
}
#endif

#ifdef __APPLE__
std::unique_ptr<HwDecodeProvider> create_videotoolbox_provider() {
    return std::make_unique<VideoToolboxProvider>();
}
#endif

std::vector<HwDecodeProviderDescriptor> registered_hw_decode_providers() {
    std::vector<HwDecodeProviderDescriptor> providers;
#ifdef _WIN32
    providers.push_back({
        "D3D12VA",
        RenderBackendKind::WgpuD3D12,
        RenderBackendKind::Unknown,
        false,
        &create_d3d12va_provider,
    });
#endif
#ifdef __APPLE__
    providers.push_back({
        "VideoToolbox",
        RenderBackendKind::WgpuMetal,
        RenderBackendKind::Unknown,
        true,
        &create_videotoolbox_provider,
    });
#endif
    return providers;
}

bool provider_matches_request(const HwDecodeProviderDescriptor& provider,
                              const HwDecodeInitParams& params) {
    if (provider.backend == params.backend) {
        return true;
    }
    if (provider.secondary_backend == params.backend) {
        return true;
    }
    return provider.allow_ffmpeg_owned_hwdownload &&
           params.device_mode == DecodeDeviceMode::FfmpegOwnedHwDownloadDevice;
}

} // namespace

HwDecodeInitResult try_hw_decode_providers(
    const AVCodec* codec,
    const HwDecodeInitParams& params)
{
    if (!codec) {
        spdlog::debug("[HWDecode] Skipping: no codec");
        return {};
    }

    std::vector<std::unique_ptr<HwDecodeProvider>> providers;
    for (const auto& descriptor : registered_hw_decode_providers()) {
        if (!descriptor.create || !provider_matches_request(descriptor, params)) {
            continue;
        }
        providers.push_back(descriptor.create());
    }

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
    case HwDecodeType::D3D12VA:
        return "D3D12VA";
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

std::vector<const char*> compatible_hw_decode_provider_names(
    RenderBackendKind backend,
    DecodeDeviceMode device_mode) {
    HwDecodeInitParams params;
    params.backend = backend;
    params.device_mode = device_mode;

    std::vector<const char*> names;
    for (const auto& descriptor : registered_hw_decode_providers()) {
        if (provider_matches_request(descriptor, params)) {
            names.push_back(descriptor.name);
        }
    }
    return names;
}

} // namespace vr
