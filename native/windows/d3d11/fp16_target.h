#pragma once

#include <d3d11.h>
#include <cstdint>
#include <vector>
#include <wrl/client.h>

namespace vr {

class D3D11Fp16Target {
public:
    bool initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        int width,
        int height);
    bool resize(int width, int height);
    void shutdown();

    ID3D11RenderTargetView* rtv() const { return rtv_.Get(); }
    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    int width() const { return width_; }
    int height() const { return height_; }
    uint64_t estimated_bytes() const;

    bool capture_rgba16f(
        std::vector<uint16_t>& rgba_half,
        int& width,
        int& height) const;

private:
    bool create_resources(int width, int height);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    int width_ = 0;
    int height_ = 0;
};

} // namespace vr
