#pragma once

#include "renderer/decode/hw/hw_decode_provider.h"

namespace vr {

class VideoToolboxProvider : public HwDecodeProvider {
public:
    VideoToolboxProvider() = default;
    ~VideoToolboxProvider() override;

    VideoToolboxProvider(const VideoToolboxProvider&) = delete;
    VideoToolboxProvider& operator=(const VideoToolboxProvider&) = delete;

    bool probe(const AVCodec* codec) const override;
    HwDecodeInitResult init(const HwDecodeInitParams& params) override;
    void shutdown() override;
    void flush() override;
    HwDecodeType type() const override { return HwDecodeType::VideoToolbox; }
    const char* name() const override { return "VideoToolbox"; }
};

} // namespace vr
