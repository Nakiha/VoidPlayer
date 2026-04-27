#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/d3d11/device.h"
#include "video_renderer/d3d11/shader.h"

using namespace vr::test;

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

TEST_CASE("ShaderManager creates constant buffer of 256 bytes", "[d3d11][shader]") {
    auto [dev, hwnd] = create_test_device();
    vr::ShaderManager sm(dev->device());

    vr::CompiledShader shader;
    bool result = sm.create_constant_buffer(dev->device(), 256, shader);

    REQUIRE(result == true);
    REQUIRE(shader.constant_buffer != nullptr);

    // Verify buffer description
    D3D11_BUFFER_DESC desc = {};
    shader.constant_buffer->GetDesc(&desc);
    REQUIRE(desc.ByteWidth == 256);
    REQUIRE(desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER);

    cleanup_test_device(dev, hwnd);
}
