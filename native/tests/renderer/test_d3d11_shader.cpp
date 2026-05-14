#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/d3d11/device.h"
#include "video_renderer/d3d11/shader.h"
#include "video_renderer/shader_constants.h"
#include <cstddef>

using namespace vr::test;

namespace vr {
uint32_t pack_overlay_uv16(int a, int a_extent, int b, int b_extent);
}

TEST_CASE("ShaderManager compiles trivial VS and PS from source", "[d3d11][shader]") {
    auto [dev, hwnd] = create_test_device();
    vr::ShaderManager sm(dev->device());

    // Vertex shader: pass-through position
    const std::string vs_source = R"(
struct VS_INPUT {
    float2 pos : POSITION;
    float2 tex : TEXCOORD0;
};

float4 main(VS_INPUT input) : SV_POSITION {
    return float4(input.pos, 0.0, 1.0);
}
)";

    // Pixel shader: solid red output
    const std::string ps_source = R"(
float4 main(float4 pos : SV_POSITION) : SV_TARGET {
    return float4(1.0, 0.0, 0.0, 1.0);
}
)";

    // Combine into a single source for both stages
    const std::string combined = R"(
struct VS_INPUT {
    float2 pos : POSITION;
    float2 tex : TEXCOORD0;
};

float4 vs_main(VS_INPUT input) : SV_POSITION {
    return float4(input.pos, 0.0, 1.0);
}

float4 ps_main(float4 pos : SV_POSITION) : SV_TARGET {
    return float4(1.0, 0.0, 0.0, 1.0);
}
)";

    vr::CompiledShader shader;
    bool result = sm.compile_from_source(combined, "vs_main", "ps_main", shader);

    REQUIRE(result == true);
    REQUIRE(shader.vs != nullptr);
    REQUIRE(shader.ps != nullptr);

    cleanup_test_device(dev, hwnd);
}

TEST_CASE("ShaderManager creates input layout", "[d3d11][shader]") {
    auto [dev, hwnd] = create_test_device();
    vr::ShaderManager sm(dev->device());

    const std::string source = R"(
struct VS_INPUT {
    float2 pos : POSITION;
    float2 tex : TEXCOORD0;
};

float4 vs_main(VS_INPUT input) : SV_POSITION {
    return float4(input.pos, 0.0, 1.0);
}

float4 ps_main(float4 pos : SV_POSITION) : SV_TARGET {
    return float4(1.0, 1.0, 1.0, 1.0);
}
)";

    vr::CompiledShader shader;
    bool result = sm.compile_from_source(source, "vs_main", "ps_main", shader);

    REQUIRE(result == true);
    REQUIRE(shader.layout != nullptr);

    cleanup_test_device(dev, hwnd);
}

TEST_CASE("ShaderManager garbage source returns false", "[d3d11][shader]") {
    auto [dev, hwnd] = create_test_device();
    vr::ShaderManager sm(dev->device());

    const std::string garbage = "this is not valid HLSL code at all!!!";

    vr::CompiledShader shader;
    bool result = sm.compile_from_source(garbage, "vs_main", "ps_main", shader);

    REQUIRE(result == false);
    // Verify we didn't crash and the pointers are null
    REQUIRE(shader.vs == nullptr);
    REQUIRE(shader.ps == nullptr);

    cleanup_test_device(dev, hwnd);
}

TEST_CASE("Shader constants layout matches HLSL cbuffer", "[d3d11][shader]") {
    REQUIRE(sizeof(vr::ShaderConstants) == vr::kShaderConstantsSize);
    REQUIRE(offsetof(vr::ShaderConstants, mode) == 0);
    REQUIRE(offsetof(vr::ShaderConstants, canvas_width) == 16);
    REQUIRE(offsetof(vr::ShaderConstants, order) == 32);
    REQUIRE(offsetof(vr::ShaderConstants, video_aspect) == 48);
    REQUIRE(offsetof(vr::ShaderConstants, nv12_mask) == 64);
    REQUIRE(offsetof(vr::ShaderConstants, nv12_uv_scale_y) == 80);
    REQUIRE(offsetof(vr::ShaderConstants, track_scale) == 112);
    REQUIRE(offsetof(vr::ShaderConstants, display_offset_x) == 128);
    REQUIRE(offsetof(vr::ShaderConstants, background_color) == 224);
    REQUIRE(offsetof(vr::ShaderConstants, color_range) == 240);
    REQUIRE(offsetof(vr::ShaderConstants, color_primaries) == 288);
}

TEST_CASE("Analysis overlay rect UV packing is stable at tiny and high resolutions",
          "[d3d11][shader][analysis][overlay]") {
    auto low16 = [](uint32_t packed) {
        return packed & 0xffffu;
    };
    auto high16 = [](uint32_t packed) {
        return packed >> 16;
    };

    const uint32_t one_pixel_min = vr::pack_overlay_uv16(0, 1, 0, 1);
    const uint32_t one_pixel_max = vr::pack_overlay_uv16(1, 1, 1, 1);
    REQUIRE(low16(one_pixel_min) == 0);
    REQUIRE(high16(one_pixel_min) == 0);
    REQUIRE(low16(one_pixel_max) == 65535);
    REQUIRE(high16(one_pixel_max) == 65535);

    const uint32_t two_pixel_mid = vr::pack_overlay_uv16(1, 2, 1, 2);
    REQUIRE(low16(two_pixel_mid) == high16(two_pixel_mid));
    REQUIRE(low16(two_pixel_mid) > low16(vr::pack_overlay_uv16(0, 2, 0, 2)));
    REQUIRE(low16(two_pixel_mid) < low16(vr::pack_overlay_uv16(2, 2, 2, 2)));

    const int width_8k = 7680;
    const int height_8k = 4320;
    REQUIRE(low16(vr::pack_overlay_uv16(0, width_8k, 0, height_8k)) == 0);
    REQUIRE(high16(vr::pack_overlay_uv16(0, width_8k, 0, height_8k)) == 0);
    REQUIRE(low16(vr::pack_overlay_uv16(width_8k, width_8k, height_8k, height_8k)) == 65535);
    REQUIRE(high16(vr::pack_overlay_uv16(width_8k, width_8k, height_8k, height_8k)) == 65535);

    const uint32_t edge_left = vr::pack_overlay_uv16(width_8k - 1, width_8k, 0, height_8k);
    const uint32_t edge_right = vr::pack_overlay_uv16(width_8k, width_8k, 0, height_8k);
    REQUIRE(low16(edge_left) < low16(edge_right));

    const uint32_t shared_a = vr::pack_overlay_uv16(3840, width_8k, 2160, height_8k);
    const uint32_t shared_b = vr::pack_overlay_uv16(3840, width_8k, 2160, height_8k);
    REQUIRE(shared_a == shared_b);

    const uint32_t clamped_low = vr::pack_overlay_uv16(-20, width_8k, -10, height_8k);
    const uint32_t clamped_high =
        vr::pack_overlay_uv16(width_8k + 20, width_8k, height_8k + 10, height_8k);
    REQUIRE(low16(clamped_low) == 0);
    REQUIRE(high16(clamped_low) == 0);
    REQUIRE(low16(clamped_high) == 65535);
    REQUIRE(high16(clamped_high) == 65535);
}

TEST_CASE("ShaderManager creates renderer constant buffer", "[d3d11][shader]") {
    auto [dev, hwnd] = create_test_device();
    vr::ShaderManager sm(dev->device());

    vr::CompiledShader shader;
    bool result = sm.create_constant_buffer(
        dev->device(), static_cast<UINT>(vr::kShaderConstantsSize), shader);

    REQUIRE(result == true);
    REQUIRE(shader.constant_buffer != nullptr);

    // Verify buffer description
    D3D11_BUFFER_DESC desc = {};
    shader.constant_buffer->GetDesc(&desc);
    REQUIRE(desc.ByteWidth == vr::kShaderConstantsSize);
    REQUIRE(desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER);

    cleanup_test_device(dev, hwnd);
}
