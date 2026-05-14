#include "video_renderer/d3d11/render_backend.h"

#include "embedded_shaders.h"
#include "video_renderer/shader_constants.h"

#include <dxgi.h>
#include <spdlog/spdlog.h>
#include <vector>

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
    const std::vector<EmbeddedShaderFile> multitrack_includes = {
        {"common.hlsl", kCommonHlsl, sizeof(kCommonHlsl) - 1},
        {"color_pipeline.hlsl", kColorPipelineHlsl, sizeof(kColorPipelineHlsl) - 1},
        {"sampling.hlsl", kSamplingHlsl, sizeof(kSamplingHlsl) - 1},
    };
    if (!shader_manager_->compile_from_source(
            kMultitrackHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->compiled_shader)) {
        spdlog::error("Renderer: failed to compile shaders");
        return false;
    }

    if (!shader_manager_->compile_from_source(
            kAnalysisOverlayHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->overlay_shader)) {
        spdlog::error("Renderer: failed to compile overlay shaders");
        return false;
    }

    if (!shader_manager_->compile_from_source(
            kAnalysisOverlayInvertHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->overlay_invert_shader)) {
        spdlog::error("Renderer: failed to compile overlay invert shaders");
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

    D3D11_SAMPLER_DESC overlay_sampler_desc = sampler_desc;
    overlay_sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = device_->device()->CreateSamplerState(
        &overlay_sampler_desc, &resources_->overlay_sampler_state);
    if (FAILED(hr) || !resources_->overlay_sampler_state) {
        spdlog::error("Renderer: CreateSamplerState(overlay) failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->device()->CreateBlendState(
        &blend_desc, &resources_->overlay_blend_state);
    if (FAILED(hr) || !resources_->overlay_blend_state) {
        spdlog::error("Renderer: CreateBlendState(overlay) failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_BLEND_DESC invert_blend_desc = {};
    invert_blend_desc.RenderTarget[0].BlendEnable = TRUE;
    invert_blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    invert_blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    invert_blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_SUBTRACT;
    invert_blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    invert_blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    invert_blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    invert_blend_desc.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED |
        D3D11_COLOR_WRITE_ENABLE_GREEN |
        D3D11_COLOR_WRITE_ENABLE_BLUE;
    hr = device_->device()->CreateBlendState(
        &invert_blend_desc, &resources_->overlay_invert_blend_state);
    if (FAILED(hr) || !resources_->overlay_invert_blend_state) {
        spdlog::error("Renderer: CreateBlendState(overlay invert) failed: HRESULT {:#x}",
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
