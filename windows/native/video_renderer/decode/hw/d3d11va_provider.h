#pragma once
#include "video_renderer/decode/hw/hw_decode_provider.h"
#include <mutex>
#include <memory>

namespace vr {

class D3D11VAProvider : public HwDecodeProvider {
public:
    D3D11VAProvider() = default;
    ~D3D11VAProvider() override;

    D3D11VAProvider(const D3D11VAProvider&) = delete;
    D3D11VAProvider& operator=(const D3D11VAProvider&) = delete;

    bool probe(const AVCodec* codec) const override;
    HwDecodeInitResult init(void* native_device, int width, int height,
                            std::recursive_mutex* device_mutex = nullptr) override;
    void shutdown() override;
    HwDecodeType type() const override { return HwDecodeType::D3D11VA; }
    const char* name() const override { return "D3D11VA"; }

private:
    // Mutex must outlive the AVBufferRef because FFmpeg calls lock/unlock
    // during context teardown. Held as member to ensure lifetime.
    std::unique_ptr<std::recursive_mutex> device_mutex_;

    // Pixel format found during probe (AV_PIX_FMT_D3D11VA_VLD or AV_PIX_FMT_D3D11)
    mutable AVPixelFormat probed_pix_fmt_ = AV_PIX_FMT_NONE;
};

} // namespace vr
