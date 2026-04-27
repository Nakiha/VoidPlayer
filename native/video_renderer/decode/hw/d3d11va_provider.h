#pragma once
#include "video_renderer/decode/hw/hw_decode_provider.h"
#include <mutex>
#include <memory>
#include <d3d11.h>
#include <wrl/client.h>

namespace vr {

class D3D11VAProvider : public HwDecodeProvider {
public:
    D3D11VAProvider() = default;
    ~D3D11VAProvider() override;

    D3D11VAProvider(const D3D11VAProvider&) = delete;
    D3D11VAProvider& operator=(const D3D11VAProvider&) = delete;

    bool probe(const AVCodec* codec) const override;
    HwDecodeInitResult init(const HwDecodeInitParams& params) override;
    void shutdown() override;
    void flush() override;
    HwDecodeType type() const override { return HwDecodeType::D3D11VA; }
    const char* name() const override { return "D3D11VA"; }

private:
    // Mutex must outlive the AVBufferRef because FFmpeg calls lock/unlock
    // during context teardown. Held as member to ensure lifetime.
    std::unique_ptr<std::recursive_mutex> device_mutex_;
    std::recursive_mutex* active_mutex_ = nullptr;
    bool uses_shared_device_ = false;

    // Independent D3D11 device for hardware decoding (created when no external
    // device is provided). Keeping these alive prevents premature release during
    // FFmpeg's teardown.
    Microsoft::WRL::ComPtr<ID3D11Device> own_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;

    // Pixel format found during probe (AV_PIX_FMT_D3D11VA_VLD or AV_PIX_FMT_D3D11)
    mutable AVPixelFormat probed_pix_fmt_ = AV_PIX_FMT_NONE;
    mutable AVCodecID probed_codec_id_ = AV_CODEC_ID_NONE;
};

} // namespace vr
