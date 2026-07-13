#pragma once

#include "renderer/decode/hw/hw_decode_provider.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>

namespace vr {

class D3D11VAProvider final : public HwDecodeProvider {
public:
    D3D11VAProvider() = default;
    ~D3D11VAProvider() override;

    D3D11VAProvider(const D3D11VAProvider&) = delete;
    D3D11VAProvider& operator=(const D3D11VAProvider&) = delete;

    bool probe(const AVCodec* codec) const override;
    HwDecodeInitResult init(const HwDecodeInitParams& params) override;
    void shutdown() override;
    void flush() override;
    void wait_idle() override;
    HwDecodeType type() const override { return HwDecodeType::D3D11VA; }
    const char* name() const override { return "D3D11VA"; }

private:
    std::unique_ptr<std::recursive_mutex> device_mutex_;
    std::recursive_mutex* active_mutex_ = nullptr;
    bool uses_shared_device_ = false;
    Microsoft::WRL::ComPtr<ID3D11Device> owned_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> device_context_;
    mutable AVPixelFormat probed_pixel_format_ = AV_PIX_FMT_NONE;
    mutable AVCodecID probed_codec_id_ = AV_CODEC_ID_NONE;
};

} // namespace vr
