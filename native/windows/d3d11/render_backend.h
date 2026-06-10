#pragma once

#include "renderer/render/presentation_backend.h"
#include "windows/d3d11/device.h"
#include "windows/d3d11/frame_presenter.h"
#include "windows/d3d11/headless_output.h"
#include "windows/d3d11/shader.h"
#include "windows/d3d11/texture.h"

#include <array>
#include <memory>
#include <wrl/client.h>

namespace vr {

using D3D11RenderBackendConfig = PresentationBackendConfig;

struct D3D11RenderResources {
    CompiledShader compiled_shader;
    CompiledShader overlay_shader;
    CompiledShader overlay_invert_shader;
    CompiledShader overlay_contrast_shader;
    CompiledShader overlay_rect_shader;
    CompiledShader overlay_mask_rect_shader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_state;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> overlay_sampler_state;
    Microsoft::WRL::ComPtr<ID3D11BlendState> overlay_blend_state;
    Microsoft::WRL::ComPtr<ID3D11BlendState> overlay_invert_blend_state;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 4> overlay_textures;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> overlay_srvs;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 4> overlay_mask_textures;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> overlay_mask_srvs;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, 4> overlay_mask_rtvs;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 4> overlay_rect_buffers;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> overlay_rect_srvs;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> cached_rtv;
    std::array<int, 4> overlay_width = {0, 0, 0, 0};
    std::array<int, 4> overlay_height = {0, 0, 0, 0};
    std::array<int, 4> overlay_mask_width = {0, 0, 0, 0};
    std::array<int, 4> overlay_mask_height = {0, 0, 0, 0};
    std::array<uint32_t, 4> overlay_rect_capacity = {0, 0, 0, 0};
};

class D3D11RenderBackend : public PresentationBackend {
public:
    D3D11RenderBackend() = default;
    ~D3D11RenderBackend() override;

    D3D11RenderBackend(const D3D11RenderBackend&) = delete;
    D3D11RenderBackend& operator=(const D3D11RenderBackend&) = delete;

    PresentationBackendKind kind() const override { return PresentationBackendKind::D3D11; }
    const char* name() const override { return "d3d11"; }
    bool initialize(const PresentationBackendConfig& config) override;
    void shutdown() override;

    bool headless() const override { return headless_; }
    bool renderer_manages_headless_publish() const override { return true; }
    bool supports_swap_chain_present() const override;
    bool poll_device_removed(const char* operation) override;
    bool device_lost() const override;
    long device_removed_reason() const override;
    void wait_idle(const char* label) override;
    bool present_swap_chain(int sync_interval) override;
    void reset_track(size_t slot) override;
    void move_track(size_t from, size_t to) override;
    bool begin_renderer_managed_headless_frame() override;
    std::function<void()> publish_renderer_managed_headless_frame(
        const char* label) override;
    bool resize_renderer_managed_headless_output(int width, int height) override;
    void cleanup_renderer_managed_headless_pending_buffers() override;
    bool set_renderer_managed_headless_frame_callback(
        std::function<void()> callback) override;

    D3D11Device* device() const { return device_.get(); }
    TextureManager* texture_manager() const { return texture_manager_.get(); }
    D3D11FramePresenter* frame_presenter() const { return frame_presenter_.get(); }
    D3D11HeadlessOutput* headless_output() const { return headless_output_.get(); }
    ShaderManager* shader_manager() const { return shader_manager_.get(); }
    D3D11RenderResources* resources() const { return resources_.get(); }
    bool capture_front_buffer(std::vector<uint8_t>& bgra,
                              int& width,
                              int& height) override;
    bool capture_front_buffer_region(int x,
                                     int y,
                                     int width,
                                     int height,
                                     std::vector<uint8_t>& bgra,
                                     int& region_width,
                                     int& region_height) override;
    bool draw_frame(const RendererDrawSnapshot& snapshot,
                    const PresentationBackendDrawHooks& hooks) override;

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
