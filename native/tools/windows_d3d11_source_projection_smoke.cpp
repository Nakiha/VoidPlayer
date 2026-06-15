#include "windows/presentation/windows_dcomp_composite.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

struct CompositeConstants {
    float viewport[4];
    float sdr_white_scale;
    float output_mode;
    float source_projection_enabled;
    float source_mode;
    float source_split_pos;
    float source_track_count;
    float source_header_padding[2];
    float source_present[4];
    float source_order[4];
    float source_transfer[4];
    float source_display_offset_x[4];
    float source_display_offset_y[4];
    float source_inv_display_size_x[4];
    float source_inv_display_size_y[4];
    float source_view_offset_uv_x[4];
    float source_view_offset_uv_y[4];
    float background_color[4];
};

uint16_t float_to_half(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16u) & 0x8000u;
    int exponent = static_cast<int>((bits >> 23u) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa = (mantissa | 0x800000u) >>
                   static_cast<uint32_t>(1 - exponent);
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

bool approximately_equal(float actual, float expected) {
    return std::abs(actual - expected) <= 0.03f;
}

} // namespace

int main() {
    constexpr UINT width = 4;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = {};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &device, &level, &context);
    }
    if (FAILED(hr)) return 1;

    D3D11_TEXTURE2D_DESC source_desc = {};
    source_desc.Width = 1;
    source_desc.Height = 1;
    source_desc.MipLevels = 1;
    source_desc.ArraySize = 1;
    source_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    source_desc.SampleDesc.Count = 1;
    source_desc.Usage = D3D11_USAGE_IMMUTABLE;
    source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const std::array<std::array<float, 4>, 4> colors = {{
        {2.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 3.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 4.0f, 1.0f},
        {5.0f, 5.0f, 5.0f, 1.0f},
    }};
    std::array<ComPtr<ID3D11Texture2D>, 4> source_textures;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> source_srvs;
    for (size_t slot = 0; slot < colors.size(); ++slot) {
        std::array<uint16_t, 4> pixel = {
            float_to_half(colors[slot][0]),
            float_to_half(colors[slot][1]),
            float_to_half(colors[slot][2]),
            float_to_half(colors[slot][3]),
        };
        D3D11_SUBRESOURCE_DATA data = {pixel.data(), 8, 0};
        if (FAILED(device->CreateTexture2D(
                &source_desc, &data, &source_textures[slot])) ||
            FAILED(device->CreateShaderResourceView(
                source_textures[slot].Get(), nullptr, &source_srvs[slot]))) {
            return 2;
        }
    }

    D3D11_TEXTURE2D_DESC output_desc = source_desc;
    output_desc.Width = width;
    output_desc.Usage = D3D11_USAGE_DEFAULT;
    output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11RenderTargetView> output_rtv;
    if (FAILED(device->CreateTexture2D(
            &output_desc, nullptr, &output)) ||
        FAILED(device->CreateRenderTargetView(
            output.Get(), nullptr, &output_rtv))) {
        return 3;
    }

    const char* shader = vr::windows_dcomp_composite_hlsl();
    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(
            shader, std::strlen(shader), nullptr, nullptr, nullptr,
            "VSMain", "vs_5_0", 0, 0, &vs_blob, &errors)) ||
        FAILED(D3DCompile(
            shader, std::strlen(shader), nullptr, nullptr, nullptr,
            "PSVideo", "ps_5_0", 0, 0, &ps_blob, &errors))) {
        return 4;
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    if (FAILED(device->CreateVertexShader(
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
            nullptr, &vs)) ||
        FAILED(device->CreatePixelShader(
            ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
            nullptr, &ps))) {
        return 5;
    }
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    if (FAILED(device->CreateSamplerState(&sampler_desc, &sampler))) return 6;

    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.ByteWidth = sizeof(CompositeConstants);
    cb_desc.Usage = D3D11_USAGE_DEFAULT;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;
    if (FAILED(device->CreateBuffer(
            &cb_desc, nullptr, &constants))) return 7;
    CompositeConstants values = {};
    values.viewport[2] = 1.0f;
    values.viewport[3] = 1.0f;
    values.sdr_white_scale = 1.0f;
    values.output_mode = 1.0f;
    values.source_projection_enabled = 1.0f;
    values.source_track_count = 4.0f;
    const std::array<float, 4> order = {2.0f, 0.0f, 3.0f, 1.0f};
    for (size_t i = 0; i < 4; ++i) {
        values.source_present[i] = 1.0f;
        values.source_order[i] = order[i];
        values.source_inv_display_size_x[i] = 1.0f;
        values.source_inv_display_size_y[i] = 1.0f;
    }
    context->UpdateSubresource(
        constants.Get(), 0, nullptr, &values, 0, 0);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = 1.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* rtv = output_rtv.Get();
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {
        nullptr, nullptr,
        source_srvs[0].Get(), source_srvs[1].Get(),
        source_srvs[2].Get(), source_srvs[3].Get(),
    };
    context->PSSetShaderResources(0, 6, srvs);
    ID3D11SamplerState* raw_sampler = sampler.Get();
    context->PSSetSamplers(0, 1, &raw_sampler);
    ID3D11Buffer* raw_constants = constants.Get();
    context->PSSetConstantBuffers(0, 1, &raw_constants);
    context->Draw(4, 0);

    D3D11_TEXTURE2D_DESC staging_desc = output_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(
            &staging_desc, nullptr, &staging))) return 8;
    context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(
            staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 9;
    const auto* pixels = static_cast<const uint16_t*>(mapped.pData);
    const std::array<int, 4> expected_slots = {2, 0, 3, 1};
    bool ok = true;
    for (UINT x = 0; x < width; ++x) {
        const auto& expected = colors[expected_slots[x]];
        for (size_t channel = 0; channel < 3; ++channel) {
            ok = approximately_equal(
                vr::half_to_float(pixels[x * 4u + channel]),
                expected[channel]) && ok;
        }
    }
    context->Unmap(staging.Get(), 0);
    if (!ok) return 10;
    std::printf(
        "windows_d3d11_source_projection_smoke passed; "
        "four-track order and >1.0 scRGB retained\n");
    return 0;
}
