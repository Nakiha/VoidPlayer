#include "windows/d3d11/fp16_target.h"

#include "windows/d3d11/memory_estimate.h"

#include <cstring>
#include <spdlog/spdlog.h>

namespace vr {

bool D3D11Fp16Target::initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    int width,
    int height) {
    shutdown();
    device_ = device;
    context_ = context;
    if (!device_ || !context_ || !create_resources(width, height)) {
        shutdown();
        return false;
    }
    return true;
}

bool D3D11Fp16Target::resize(int width, int height) {
    if (!device_ || !context_) {
        return false;
    }
    texture_.Reset();
    rtv_.Reset();
    srv_.Reset();
    width_ = 0;
    height_ = 0;
    return create_resources(width, height);
}

void D3D11Fp16Target::shutdown() {
    srv_.Reset();
    rtv_.Reset();
    texture_.Reset();
    width_ = 0;
    height_ = 0;
    context_ = nullptr;
    device_ = nullptr;
}

uint64_t D3D11Fp16Target::estimated_bytes() const {
    if (!texture_) {
        return 0;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    texture_->GetDesc(&desc);
    return estimate_d3d11_texture_bytes(desc);
}

bool D3D11Fp16Target::capture_rgba16f(
    std::vector<uint16_t>& rgba_half,
    int& width,
    int& height) const {
    rgba_half.clear();
    width = 0;
    height = 0;
    if (!device_ || !context_ || !texture_) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture_->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    HRESULT hr =
        device_->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        spdlog::error(
            "[D3D11Fp16Target] staging texture creation failed: HRESULT {:#x}",
            static_cast<unsigned long>(hr));
        return false;
    }
    context_->CopyResource(staging.Get(), texture_.Get());
    context_->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        spdlog::error(
            "[D3D11Fp16Target] capture Map failed: HRESULT {:#x}",
            static_cast<unsigned long>(hr));
        return false;
    }

    width = static_cast<int>(desc.Width);
    height = static_cast<int>(desc.Height);
    const size_t row_values = static_cast<size_t>(width) * 4;
    const size_t row_bytes = row_values * sizeof(uint16_t);
    rgba_half.resize(row_values * static_cast<size_t>(height));
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    auto* destination =
        reinterpret_cast<uint8_t*>(rgba_half.data());
    for (int row = 0; row < height; ++row) {
        std::memcpy(
            destination + static_cast<size_t>(row) * row_bytes,
            source + static_cast<size_t>(row) * mapped.RowPitch,
            row_bytes);
    }
    context_->Unmap(staging.Get(), 0);
    return true;
}

bool D3D11Fp16Target::create_resources(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture_);
    if (FAILED(hr) || !texture_) {
        spdlog::error(
            "[D3D11Fp16Target] texture creation failed: HRESULT {:#x}",
            static_cast<unsigned long>(hr));
        return false;
    }
    hr = device_->CreateRenderTargetView(texture_.Get(), nullptr, &rtv_);
    if (FAILED(hr) || !rtv_) {
        spdlog::error(
            "[D3D11Fp16Target] RTV creation failed: HRESULT {:#x}",
            static_cast<unsigned long>(hr));
        return false;
    }
    hr = device_->CreateShaderResourceView(texture_.Get(), nullptr, &srv_);
    if (FAILED(hr) || !srv_) {
        spdlog::error(
            "[D3D11Fp16Target] SRV creation failed: HRESULT {:#x}",
            static_cast<unsigned long>(hr));
        return false;
    }
    width_ = width;
    height_ = height;
    spdlog::info(
        "[D3D11Fp16Target] initialized {}x{} R16G16B16A16_FLOAT",
        width,
        height);
    return true;
}

} // namespace vr
