#include <catch2/catch_test_macros.hpp>

#include "windows/decode/d3d11va_provider.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <string>

using namespace vr;

TEST_CASE("D3D11VA provider exposes the pinned FFmpeg H264 hardware config",
          "[windows_d3d11va][hw_decode]") {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    REQUIRE(codec != nullptr);

    D3D11VAProvider provider;
    REQUIRE(provider.probe(codec));
    REQUIRE(provider.type() == HwDecodeType::D3D11VA);
    REQUIRE(std::string(provider.name()) == "D3D11VA");
    REQUIRE(std::string(hw_decode_type_name(provider.type())) == "D3D11VA");
}
