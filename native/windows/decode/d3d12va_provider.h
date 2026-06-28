#pragma once

#include "renderer/decode/hw/hw_decode_provider.h"

#include <memory>
#include <mutex>

namespace vr {

class D3D12VAProvider : public HwDecodeProvider {
public:
    D3D12VAProvider() = default;
    ~D3D12VAProvider() override;

    D3D12VAProvider(const D3D12VAProvider&) = delete;
    D3D12VAProvider& operator=(const D3D12VAProvider&) = delete;

    bool probe(const AVCodec* codec) const override;
    HwDecodeInitResult init(const HwDecodeInitParams& params) override;
    void shutdown() override;
    void flush() override;
    void wait_idle() override;
    HwDecodeType type() const override { return HwDecodeType::D3D12VA; }
    const char* name() const override { return "D3D12VA"; }

private:
    std::unique_ptr<std::recursive_mutex> device_mutex_;
    std::recursive_mutex* active_mutex_ = nullptr;
    mutable AVPixelFormat probed_pix_fmt_ = AV_PIX_FMT_NONE;
    mutable AVCodecID probed_codec_id_ = AV_CODEC_ID_NONE;
};

} // namespace vr
