#pragma once

#include "video_renderer/d3d11/device.h"
#include "video_renderer/d3d11/frame_presenter.h"
#include "video_renderer/d3d11/headless_output.h"
#include "video_renderer/d3d11/shader.h"
#include "video_renderer/d3d11/texture.h"

#include <array>
#include <memory>
#include <wrl/client.h>

namespace vr {

struct D3D11RenderBackendConfig {
    void* hwnd = nullptr;
    void* adapter = nullptr;
    int width = 0;
    int height = 0;
    bool headless = false;
};

struct D3D11RenderResources {
    CompiledShader compiled_shader;
    CompiledShader overlay_shader;
    CompiledShader overlay_invert_shader;
    CompiledShader overlay_rect_shader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_state;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> overlay_sampler_state;
    Microsoft::WRL::ComPtr<ID3D11BlendState> overlay_blend_state;
    Microsoft::WRL::ComPtr<ID3D11BlendState> overlay_invert_blend_state;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 4> overlay_textures;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> overlay_srvs;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 4> overlay_mask_textures;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> overlay_mask_srvs;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 4> overlay_rect_buffers;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> overlay_rect_srvs;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> cached_rtv;
    std::array<int, 4> overlay_width = {0, 0, 0, 0};
    std::array<int, 4> overlay_height = {0, 0, 0, 0};
    std::array<uint32_t, 4> overlay_rect_capacity = {0, 0, 0, 0};
};

class D3D11RenderBackend {
public:
    D3D11RenderBackend() = default;
    ~D3D11RenderBackend();

    D3D11RenderBackend(const D3D11RenderBackend&) = delete;
    D3D11RenderBackend& operator=(const D3D11RenderBackend&) = delete;

    bool initialize(const D3D11RenderBackendConfig& config);
    void shutdown();

    bool headless() const { return headless_; }

    D3D11Device* device() const { return device_.get(); }
    TextureManager* texture_manager() const { return texture_manager_.get(); }
    D3D11FramePresenter* frame_presenter() const { return frame_presenter_.get(); }
    D3D11HeadlessOutput* headless_output() const { return headless_output_.get(); }
    ShaderManager* shader_manager() const { return shader_manager_.get(); }
    D3D11RenderResources* resources() const { return resources_.get(); }

private:
    bool initialize_device(const D3D11RenderBackendConfig& config);
    bool initialize_render_resources();

    bool headless_ = false;
    std::unique_ptr<D3D11Device> device_;
    std::unique_ptr<TextureManager> texture_manager_;
    std::unique_ptr<D3D11FramePresenter> frame_presenter_;
    std::unique_ptr<D3D11HeadlessOutput> headless_output_;
    std::unique_ptr<ShaderManager> shader_manager_;
    std::unique_ptr<D3D11RenderResources> resources_;
};

} // namespace vr
