#include "video_renderer/d3d11/render_backend.h"

#include "embedded_shaders.h"
#include "video_renderer/render/presentation_backend_factory.h"
#include "video_renderer/render/presentation_snapshot.h"
#include "video_renderer/render/shader_constants.h"

#include <array>
#include <chrono>
#include <dxgi.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <vector>

namespace vr {

D3D11RenderBackend::~D3D11RenderBackend() {
    shutdown();
}

std::unique_ptr<PresentationBackend> create_presentation_backend(
    RenderBackendKind kind) {
    if (kind == RenderBackendKind::D3D11) {
        return std::make_unique<D3D11RenderBackend>();
    }
    return nullptr;
}

bool D3D11RenderBackend::initialize(const PresentationBackendConfig& config) {
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

    if (!shader_manager_->compile_from_source(
            kAnalysisOverlayContrastHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->overlay_contrast_shader)) {
        spdlog::error("Renderer: failed to compile overlay contrast shaders");
        return false;
    }

    if (!shader_manager_->compile_from_source(
            kAnalysisOverlayRectHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->overlay_rect_shader)) {
        spdlog::error("Renderer: failed to compile overlay rect shaders");
        return false;
    }

    if (!shader_manager_->compile_from_source(
            kAnalysisOverlayMaskRectHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->overlay_mask_rect_shader)) {
        spdlog::error("Renderer: failed to compile overlay mask rect shaders");
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

bool D3D11RenderBackend::draw_frame(const RendererDrawSnapshot& snapshot,
                                    const PresentationBackendDrawHooks& hooks) {
    if (!resources_ || !device_) {
        return false;
    }
    const auto& decision = snapshot.decision;
    auto& resources = *resources_;
    auto* ctx = device_->context();

    if (!resources.cached_rtv) {
        if (!headless_) {
            ID3D11Texture2D* back_buffer = nullptr;
            HRESULT hr = device_->swap_chain()->GetBuffer(
                0,
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&back_buffer));
            if (FAILED(hr)) {
                spdlog::error("[Renderer] Failed to get back buffer: HRESULT {:#x}",
                              static_cast<unsigned long>(hr));
                return false;
            }
            hr = device_->device()->CreateRenderTargetView(
                back_buffer, nullptr, &resources.cached_rtv);
            back_buffer->Release();
            if (FAILED(hr)) {
                spdlog::error("[Renderer] Failed to create RTV: HRESULT {:#x}",
                              static_cast<unsigned long>(hr));
                return false;
            }
        }
    }

    if (!resources.cached_rtv) {
        return false;
    }

    ctx->ClearRenderTargetView(resources.cached_rtv.Get(), snapshot.background_color);
    ctx->OMSetRenderTargets(1, resources.cached_rtv.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(snapshot.target_width);
    vp.Height = static_cast<float>(snapshot.target_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    ID3D11Buffer* vb = resources.vertex_buffer.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    if (resources.compiled_shader.layout) {
        ctx->IASetInputLayout(resources.compiled_shader.layout.Get());
    }

    ctx->VSSetShader(resources.compiled_shader.vs.Get(), nullptr, 0);
    ctx->PSSetShader(resources.compiled_shader.ps.Get(), nullptr, 0);

    ID3D11ShaderResourceView* srvs[4] = {};
    ID3D11ShaderResourceView* nv12_y_srvs[4] = {};
    ID3D11ShaderResourceView* nv12_uv_srvs[4] = {};
    ID3D11ShaderResourceView* planar_u_srvs[4] = {};
    ID3D11ShaderResourceView* planar_v_srvs[4] = {};
    std::array<D3D11PreparedFrame, kMaxTracks> prepared_frames;
    const D3D11FramePresenter::GpuIdleWait wait_gpu_idle =
        hooks.wait_gpu_idle
            ? hooks.wait_gpu_idle
            : D3D11FramePresenter::GpuIdleWait([](const char*) {});
    if (frame_presenter_) {
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!decision.frames[i].has_value() ||
                !decision.frames[i]->texture_handle ||
                !snapshot.tracks[i].active ||
                decision.file_ids[i] != snapshot.tracks[i].file_id ||
                decision.track_generations[i] != snapshot.tracks[i].generation) {
                continue;
            }

            const auto prepare_start = std::chrono::steady_clock::now();
            const bool prepared_ok = frame_presenter_->prepare_frame(
                i,
                decision.frames[i].value(),
                snapshot.target_width,
                snapshot.target_height,
                wait_gpu_idle,
                prepared_frames[i]);
            if (hooks.record_frame_copy_us) {
                hooks.record_frame_copy_us(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - prepare_start).count()));
            }
            if (!prepared_ok) {
                continue;
            }

            srvs[i] = prepared_frames[i].rgba_srv;
            nv12_y_srvs[i] = prepared_frames[i].nv12_y_srv;
            nv12_uv_srvs[i] = prepared_frames[i].nv12_uv_srv;
            planar_u_srvs[i] = prepared_frames[i].planar_u_srv;
            planar_v_srvs[i] = prepared_frames[i].planar_v_srv;
        }
    }

    ShaderConstants cb = {};
    bool constants_ready = false;

    if (resources.compiled_shader.constant_buffer) {
        populate_layout_shader_constants(
            cb,
            snapshot.layout,
            snapshot.track_geometry,
            snapshot.target_width,
            snapshot.target_height);
        cb.nv12_mask = 0;
        cb.planar_yuv_mask = 0;
        for (int i = 0; i < 4; ++i) {
            cb.background_color[i] = snapshot.background_color[i];
        }

        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!snapshot.tracks[i].active) {
                cb.nv12_uv_scale_x[i] = 1.0f;
                cb.nv12_uv_scale_y[i] = 1.0f;
                cb.color_range[i] = VIDEO_COLOR_RANGE_LIMITED;
                cb.color_matrix[i] = VIDEO_COLOR_MATRIX_BT709;
                cb.color_transfer[i] = VIDEO_COLOR_TRANSFER_SDR;
                cb.color_primaries[i] = VIDEO_COLOR_PRIMARIES_BT709;
                continue;
            }
            const bool frame_matches_track =
                decision.file_ids[i] == snapshot.tracks[i].file_id &&
                decision.track_generations[i] == snapshot.tracks[i].generation;
            const VideoColorInfo color =
                decision.frames[i].has_value() && frame_matches_track
                    ? decision.frames[i]->color
                    : VideoColorInfo{};
            cb.color_range[i] = color.range != VIDEO_COLOR_RANGE_UNKNOWN
                ? color.range
                : VIDEO_COLOR_RANGE_LIMITED;
            cb.color_matrix[i] = color.matrix != VIDEO_COLOR_MATRIX_UNKNOWN
                ? color.matrix
                : default_presentation_color_matrix_for_size(
                    snapshot.tracks[i].video_width,
                    snapshot.tracks[i].video_height);
            cb.color_transfer[i] = color.transfer != VIDEO_COLOR_TRANSFER_UNKNOWN
                ? color.transfer
                : VIDEO_COLOR_TRANSFER_SDR;
            cb.color_primaries[i] = color.primaries != VIDEO_COLOR_PRIMARIES_UNKNOWN
                ? color.primaries
                : default_presentation_color_primaries_for_matrix(cb.color_matrix[i]);
            if (decision.frames[i].has_value() &&
                frame_matches_track &&
                decision.frames[i]->cpu_planar_yuv_storage()) {
                cb.planar_yuv_mask |= (1 << static_cast<int>(i));
                cb.nv12_uv_scale_x[i] = 1.0f;
                cb.nv12_uv_scale_y[i] = 1.0f;
            } else if (decision.frames[i].has_value() &&
                       frame_matches_track &&
                       decision.frames[i]->is_nv12) {
                cb.nv12_mask |= (1 << static_cast<int>(i));
                cb.nv12_uv_scale_x[i] = prepared_frames[i].nv12_uv_scale_x;
                cb.nv12_uv_scale_y[i] = prepared_frames[i].nv12_uv_scale_y;
            } else {
                cb.nv12_uv_scale_x[i] = prepared_frames[i].nv12_uv_scale_x;
                cb.nv12_uv_scale_y[i] = prepared_frames[i].nv12_uv_scale_y;
            }
        }
        ctx->UpdateSubresource(
            resources.compiled_shader.constant_buffer.Get(), 0, nullptr, &cb, 0, 0);
        ctx->VSSetConstantBuffers(
            0, 1, resources.compiled_shader.constant_buffer.GetAddressOf());
        ctx->PSSetConstantBuffers(
            0, 1, resources.compiled_shader.constant_buffer.GetAddressOf());
        constants_ready = true;
    }

    if (resources.sampler_state) {
        ID3D11SamplerState* sampler = resources.sampler_state.Get();
        ctx->PSSetSamplers(0, 1, &sampler);
    }

    ctx->PSSetShaderResources(0, 4, srvs);
    ctx->PSSetShaderResources(4, 4, nv12_y_srvs);
    ctx->PSSetShaderResources(8, 4, nv12_uv_srvs);
    ctx->PSSetShaderResources(12, 4, planar_u_srvs);
    ctx->PSSetShaderResources(16, 4, planar_v_srvs);

    ctx->Draw(4, 0);

    if (constants_ready && hooks.draw_overlay) {
        hooks.draw_overlay(*this, snapshot);
    }

    ID3D11ShaderResourceView* null_srvs[4] = {};
    ctx->PSSetShaderResources(0, 4, null_srvs);
    ctx->PSSetShaderResources(4, 4, null_srvs);
    ctx->PSSetShaderResources(8, 4, null_srvs);
    ctx->PSSetShaderResources(12, 4, null_srvs);
    ctx->PSSetShaderResources(16, 4, null_srvs);

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
