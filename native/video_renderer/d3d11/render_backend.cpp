#include "video_renderer/d3d11/render_backend.h"

#include "embedded_shaders.h"
#include "video_renderer/shader_constants.h"

#include <dxgi.h>
#include <spdlog/spdlog.h>

namespace vr {

D3D11RenderBackend::~D3D11RenderBackend() {
    shutdown();
}

bool D3D11RenderBackend::initialize(const D3D11RenderBackendConfig& config) {
    shutdown();
    headless_ = config.headless;

    if (!initialize_device(config)) {
        return false;
    }

    texture_manager_ = std::make_unique<TextureManager>(
        device_->device(), device_->context());
    frame_presenter_ = std::make_unique<D3D11FramePresenter>(
        texture_manager_.get(), device_->context());
    shader_manager_ = std::make_unique<ShaderManager>(device_->device());
    resources_ = std::make_unique<D3D11RenderResources>();

    return initialize_render_resources();
}

bool D3D11RenderBackend::initialize_device(const D3D11RenderBackendConfig& config) {
    device_ = std::make_unique<D3D11Device>();
    if (config.headless) {
        auto* adapter = static_cast<IDXGIAdapter*>(config.adapter);
        if (!device_->initialize_headless(adapter, config.width, config.height)) {
            spdlog::error("Renderer: failed to initialize D3D11 device (headless)");
            return false;
        }
        headless_output_ = std::make_unique<D3D11HeadlessOutput>();
        if (!headless_output_->initialize(
                device_->device(), device_->context(), config.width, config.height)) {
            return false;
        }
        return true;
    }

    if (!device_->initialize(config.hwnd, config.width, config.height)) {
        spdlog::error("Renderer: failed to initialize D3D11 device");
        return false;
    }
    return true;
}

bool D3D11RenderBackend::initialize_render_resources() {
    if (!shader_manager_->compile_from_source(
            kMultitrackHlsl, "VSMain", "PSMain", resources_->compiled_shader)) {
        spdlog::error("Renderer: failed to compile shaders");
        return false;
    }

    if (!shader_manager_->create_constant_buffer(
            device_->device(),
            static_cast<UINT>(kShaderConstantsSize),
            resources_->compiled_shader)) {
        spdlog::error("Renderer: failed to create constant buffer");
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    HRESULT hr = device_->device()->CreateSamplerState(
        &sampler_desc, &resources_->sampler_state);
    if (FAILED(hr) || !resources_->sampler_state) {
        spdlog::error("Renderer: CreateSamplerState failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    struct Vertex { float x, y, u, v; };
    Vertex quad[] = {
        {-1, -1, 0, 1},
        {-1,  1, 0, 0},
        { 1, -1, 1, 1},
        { 1,  1, 1, 0},
    };
    D3D11_BUFFER_DESC vb_desc = {};
    vb_desc.ByteWidth = sizeof(quad);
    vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA vb_data = {};
    vb_data.pSysMem = quad;
    hr = device_->device()->CreateBuffer(
        &vb_desc, &vb_data, &resources_->vertex_buffer);
    if (FAILED(hr) || !resources_->vertex_buffer) {
        spdlog::error("Renderer: CreateBuffer(vertex) failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    return true;
}

void D3D11RenderBackend::shutdown() {
    shader_manager_.reset();
    frame_presenter_.reset();
    texture_manager_.reset();
    resources_.reset();
    headless_output_.reset();

    if (device_) {
        device_->shutdown();
        device_.reset();
    }
    headless_ = false;
}

} // namespace vr
