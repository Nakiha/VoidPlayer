#include "windows/presentation/windows_dcomp_composite.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

struct CompositeConstants {
    float viewport[4];
    float sdr_white_scale;
    float padding[3];
};

uint16_t float_to_half(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16u) & 0x8000u;
    int exponent = static_cast<int>((bits >> 23u) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa = (mantissa | 0x800000u) >> static_cast<uint32_t>(1 - exponent);
        return static_cast<uint16_t>(
            sign | ((mantissa + 0x1000u) >> 13u));
    }
    if (exponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7c00u);
    }
    return static_cast<uint16_t>(
        sign | (static_cast<uint32_t>(exponent) << 10u) |
        ((mantissa + 0x1000u) >> 13u));
}

bool approximately_equal(
    float actual, float expected, float tolerance = 0.025f) {
    return std::abs(actual - expected) <= tolerance;
}

bool validate_pixel(
    const char* name,
    const std::array<float, 4>& actual,
    const vr::WindowsDcompCompositeSample& expected) {
    if (approximately_equal(actual[0], expected.r) &&
        approximately_equal(actual[1], expected.g) &&
        approximately_equal(actual[2], expected.b) &&
        approximately_equal(actual[3], expected.a)) {
        return true;
    }
    std::fprintf(
        stderr,
        "%s mismatch actual=(%.4f, %.4f, %.4f, %.4f) "
        "expected=(%.4f, %.4f, %.4f, %.4f)\n",
        name, actual[0], actual[1], actual[2], actual[3],
        expected.r, expected.g, expected.b, expected.a);
    return false;
}

} // namespace

int main() {
    constexpr UINT kWidth = 4;
    constexpr UINT kHeight = 1;
    constexpr float kSdrWhiteScale = 203.0f / 80.0f;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level = {};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &device, &feature_level, &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            &device, &feature_level, &context);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11 device creation failed: 0x%08lx\n", hr);
        return 1;
    }

    const vr::WindowsDcompCompositeSample video_sample = {
        4.0f, 0.25f, 0.5f, 1.0f};
    std::vector<uint16_t> video(kWidth * 4u);
    for (UINT x = 0; x < kWidth; ++x) {
        video[x * 4u] = float_to_half(video_sample.r);
        video[x * 4u + 1u] = float_to_half(video_sample.g);
        video[x * 4u + 2u] = float_to_half(video_sample.b);
        video[x * 4u + 3u] = float_to_half(video_sample.a);
    }
    const std::array<uint8_t, kWidth * 4u> flutter_bgra = {
        0, 0, 0, 0,
        0, 0, 128, 128,
        255, 255, 255, 255,
        32, 32, 32, 64,
    };

    D3D11_TEXTURE2D_DESC video_desc = {};
    video_desc.Width = kWidth;
    video_desc.Height = kHeight;
    video_desc.MipLevels = 1;
    video_desc.ArraySize = 1;
    video_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    video_desc.SampleDesc.Count = 1;
    video_desc.Usage = D3D11_USAGE_IMMUTABLE;
    video_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA video_data = {
        video.data(), kWidth * 8u, 0};
    ComPtr<ID3D11Texture2D> video_texture;
    if (FAILED(device->CreateTexture2D(
            &video_desc, &video_data, &video_texture))) {
        return 2;
    }

    D3D11_TEXTURE2D_DESC flutter_desc = video_desc;
    flutter_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    D3D11_SUBRESOURCE_DATA flutter_data = {
        flutter_bgra.data(), kWidth * 4u, 0};
    ComPtr<ID3D11Texture2D> flutter_texture;
    if (FAILED(device->CreateTexture2D(
            &flutter_desc, &flutter_data, &flutter_texture))) {
        return 3;
    }

    D3D11_TEXTURE2D_DESC output_desc = video_desc;
    output_desc.Usage = D3D11_USAGE_DEFAULT;
    output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11RenderTargetView> output_rtv;
    if (FAILED(device->CreateTexture2D(
            &output_desc, nullptr, &output)) ||
        FAILED(device->CreateRenderTargetView(
            output.Get(), nullptr, &output_rtv))) {
        return 4;
    }

    const char* shader = vr::windows_dcomp_composite_hlsl();
    const size_t shader_size = std::strlen(shader);
    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(
            shader, shader_size, nullptr, nullptr, nullptr,
            "VSMain", "vs_5_0", 0, 0, &vs_blob, &errors)) ||
        FAILED(D3DCompile(
            shader, shader_size, nullptr, nullptr, nullptr,
            "PSMain", "ps_5_0", 0, 0, &ps_blob, &errors))) {
        if (errors) {
            std::fwrite(
                errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        }
        return 5;
    }
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    if (FAILED(device->CreateVertexShader(
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
            nullptr, &vertex_shader)) ||
        FAILED(device->CreatePixelShader(
            ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
            nullptr, &pixel_shader))) {
        return 6;
    }

    ComPtr<ID3D11ShaderResourceView> video_srv;
    ComPtr<ID3D11ShaderResourceView> flutter_srv;
    if (FAILED(device->CreateShaderResourceView(
            video_texture.Get(), nullptr, &video_srv)) ||
        FAILED(device->CreateShaderResourceView(
            flutter_texture.Get(), nullptr, &flutter_srv))) {
        return 7;
    }
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    if (FAILED(device->CreateSamplerState(&sampler_desc, &sampler))) {
        return 8;
    }
    D3D11_BUFFER_DESC constants_desc = {};
    constants_desc.ByteWidth = sizeof(CompositeConstants);
    constants_desc.Usage = D3D11_USAGE_DEFAULT;
    constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;
    if (FAILED(device->CreateBuffer(
            &constants_desc, nullptr, &constants))) {
        return 9;
    }

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(kWidth);
    viewport.Height = static_cast<float>(kHeight);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* rtv = output_rtv.Get();
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(vertex_shader.Get(), nullptr, 0);
    context->PSSetShader(pixel_shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {video_srv.Get(), flutter_srv.Get()};
    context->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* sampler_ptr = sampler.Get();
    context->PSSetSamplers(0, 1, &sampler_ptr);
    CompositeConstants constant_values = {
        {0.0f, 0.0f, 1.0f, 1.0f}, kSdrWhiteScale, {0.0f, 0.0f, 0.0f}};
    context->UpdateSubresource(
        constants.Get(), 0, nullptr, &constant_values, 0, 0);
    ID3D11Buffer* constants_ptr = constants.Get();
    context->PSSetConstantBuffers(0, 1, &constants_ptr);
    context->Draw(4, 0);

    D3D11_TEXTURE2D_DESC staging_desc = output_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(
            &staging_desc, nullptr, &staging))) {
        return 10;
    }
    context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(
            staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return 11;
    }

    bool ok = true;
    const auto* values = static_cast<const uint16_t*>(mapped.pData);
    const std::array<vr::WindowsDcompCompositeSample, kWidth> flutter = {{
        {0.0f, 0.0f, 0.0f, 0.0f},
        {128.0f / 255.0f, 0.0f, 0.0f, 128.0f / 255.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {32.0f / 255.0f, 32.0f / 255.0f, 32.0f / 255.0f,
         64.0f / 255.0f},
    }};
    const char* names[] = {
        "transparent viewport",
        "premultiplied half-alpha edge",
        "opaque Flutter SDR white scale",
        "antialiased translucent edge",
    };
    for (UINT x = 0; x < kWidth; ++x) {
        const std::array<float, 4> actual = {
            vr::half_to_float(values[x * 4u]),
            vr::half_to_float(values[x * 4u + 1u]),
            vr::half_to_float(values[x * 4u + 2u]),
            vr::half_to_float(values[x * 4u + 3u]),
        };
        ok &= validate_pixel(
            names[x], actual,
            vr::composite_windows_dcomp_pixel(
                video_sample, flutter[x], kSdrWhiteScale));
    }
    context->Unmap(staging.Get(), 0);

    if (!ok) {
        return 12;
    }
    std::printf(
        "windows_d3d11_dcomp_flutter_composite_smoke passed; "
        "transparent native video retained %.1fx scRGB highlight\n",
        video_sample.r);
    return 0;
}
