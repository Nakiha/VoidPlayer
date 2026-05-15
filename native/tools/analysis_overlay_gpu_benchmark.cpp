#include "analysis_overlay_gpu_benchmark.h"

#include "analysis/cache/overlay_chunk.h"
#include "analysis/cache/overlay_raster.h"
#include "analysis/parsers/vachunk_parser.h"
#include "embedded_shaders.h"
#include "video_renderer/render/shader_constants.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <wrl/client.h>

namespace vr::tools {
namespace {

using Microsoft::WRL::ComPtr;

struct GpuRect {
    uint32_t rect_uv0 = 0;
    uint32_t rect_uv1 = 0;
    uint32_t color_bgra = 0;
    uint32_t track_idx = 0;
};

struct ShaderPair {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
};

struct GpuBenchmarkResult {
    bool ok = false;
    std::string error;
    std::string device_type;
    uint32_t cu_count = 0;
    uint64_t gpu_rect_upload_bytes = 0;
    double rect_upload_cpu_ms = 0.0;
    double color_pass_gpu_ms = 0.0;
    double mask_pass_gpu_ms = 0.0;
    double invert_pass_gpu_ms = 0.0;
    double full_overlay_gpu_ms = 0.0;
};

struct EmbeddedIncludeFile {
    const char* name = nullptr;
    const char* source = nullptr;
    size_t size = 0;
};

class EmbeddedInclude final : public ID3DInclude {
public:
    explicit EmbeddedInclude(std::vector<EmbeddedIncludeFile> files)
        : files_(std::move(files)) {}

    HRESULT Open(D3D_INCLUDE_TYPE,
                 LPCSTR file_name,
                 LPCVOID,
                 LPCVOID* data,
                 UINT* bytes) override {
        if (!file_name || !data || !bytes) {
            return E_INVALIDARG;
        }
        const std::string_view requested(file_name);
        for (const auto& file : files_) {
            if (requested == file.name) {
                *data = file.source;
                *bytes = static_cast<UINT>(file.size);
                return S_OK;
            }
        }
        return E_FAIL;
    }

    HRESULT Close(LPCVOID) override {
        return S_OK;
    }

private:
    std::vector<EmbeddedIncludeFile> files_;
};

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u"
                    << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch))
                    << std::dec << std::setfill(' ');
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

uint32_t pack_overlay_bgra(analysis::OverlayColor color) {
    return static_cast<uint32_t>(color.b) |
           (static_cast<uint32_t>(color.g) << 8) |
           (static_cast<uint32_t>(color.r) << 16) |
           (static_cast<uint32_t>(color.a) << 24);
}

uint32_t pack_overlay_uv16(int a, int a_extent, int b, int b_extent) {
    auto pack_one = [](int value, int extent) -> uint32_t {
        if (extent <= 0) {
            return 0;
        }
        const int clamped = std::clamp(value, 0, extent);
        return static_cast<uint32_t>(
            std::lround(static_cast<double>(clamped) * 65535.0 / static_cast<double>(extent)));
    };
    return pack_one(a, a_extent) | (pack_one(b, b_extent) << 16);
}

std::vector<GpuRect> build_rects(const analysis::VachunkOverlayFrameData& frame,
                                 uint32_t width,
                                 uint32_t height,
                                 const std::string& mode) {
    const bool qp_mode = mode == "qp";
    std::vector<GpuRect> rects;
    rects.reserve(frame.cus.size());
    for (const auto& cu : frame.cus) {
        const auto& c = cu.common;
        const int x0 = std::clamp(static_cast<int>(c.x), 0, static_cast<int>(width));
        const int y0 = std::clamp(static_cast<int>(c.y), 0, static_cast<int>(height));
        const int x1 = std::clamp(static_cast<int>(c.x + c.w), 0, static_cast<int>(width));
        const int y1 = std::clamp(static_cast<int>(c.y + c.h), 0, static_cast<int>(height));
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        GpuRect rect = {};
        rect.rect_uv0 = pack_overlay_uv16(x0, static_cast<int>(width), y0, static_cast<int>(height));
        rect.rect_uv1 = pack_overlay_uv16(x1, static_cast<int>(width), y1, static_cast<int>(height));
        rect.color_bgra = pack_overlay_bgra(
            qp_mode ? analysis::qp_color(c.qp, 255)
                    : analysis::cu_bit_density_color(c, 255));
        rect.track_idx = 0;
        rects.push_back(rect);
    }
    return rects;
}

bool compile_stage(const std::string& source,
                   const char* entry,
                   const char* target,
                   ID3DInclude* include_handler,
                   ComPtr<ID3DBlob>& blob,
                   std::string& error) {
    ComPtr<ID3DBlob> error_blob;
    HRESULT hr = D3DCompile(
        source.data(),
        source.size(),
        "AnalysisOverlayGpuBenchmark",
        nullptr,
        include_handler,
        entry,
        target,
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &blob,
        &error_blob);
    if (FAILED(hr)) {
        if (error_blob) {
            error.assign(
                static_cast<const char*>(error_blob->GetBufferPointer()),
                error_blob->GetBufferSize());
        } else {
            std::ostringstream out;
            out << "D3DCompile failed: HRESULT 0x"
                << std::hex << static_cast<unsigned long>(hr);
            error = out.str();
        }
        return false;
    }
    return true;
}

bool compile_shader_pair(ID3D11Device* device,
                         const std::string& source,
                         EmbeddedInclude& include_handler,
                         bool fullscreen_layout,
                         ShaderPair& out,
                         std::string& error) {
    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    if (!compile_stage(source, "VSMain", "vs_5_0", &include_handler, vs_blob, error) ||
        !compile_stage(source, "PSMain", "ps_5_0", &include_handler, ps_blob, error)) {
        return false;
    }
    HRESULT hr = device->CreateVertexShader(
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &out.vs);
    if (FAILED(hr)) {
        error = "CreateVertexShader failed";
        return false;
    }
    hr = device->CreatePixelShader(
        ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &out.ps);
    if (FAILED(hr)) {
        error = "CreatePixelShader failed";
        return false;
    }
    if (fullscreen_layout) {
        D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        hr = device->CreateInputLayout(
            layout_desc,
            ARRAYSIZE(layout_desc),
            vs_blob->GetBufferPointer(),
            vs_blob->GetBufferSize(),
            &out.layout);
        if (FAILED(hr)) {
            error = "CreateInputLayout failed";
            return false;
        }
    }
    return true;
}

bool wait_query(ID3D11DeviceContext* context, ID3D11Query* query, void* data, UINT size) {
    const auto start = std::chrono::steady_clock::now();
    while (context->GetData(query, data, size, 0) != S_OK) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

template <typename Fn>
bool measure_gpu_ms(ID3D11Device* device,
                    ID3D11DeviceContext* context,
                    uint32_t iterations,
                    Fn&& fn,
                    double& avg_ms,
                    std::string& error) {
    D3D11_QUERY_DESC disjoint_desc = {};
    disjoint_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    ComPtr<ID3D11Query> disjoint;
    HRESULT hr = device->CreateQuery(&disjoint_desc, &disjoint);
    if (FAILED(hr)) {
        error = "CreateQuery(timestamp disjoint) failed";
        return false;
    }
    D3D11_QUERY_DESC timestamp_desc = {};
    timestamp_desc.Query = D3D11_QUERY_TIMESTAMP;
    ComPtr<ID3D11Query> start_query;
    ComPtr<ID3D11Query> end_query;
    hr = device->CreateQuery(&timestamp_desc, &start_query);
    if (FAILED(hr)) {
        error = "CreateQuery(timestamp start) failed";
        return false;
    }
    hr = device->CreateQuery(&timestamp_desc, &end_query);
    if (FAILED(hr)) {
        error = "CreateQuery(timestamp end) failed";
        return false;
    }

    const uint32_t warmup = std::min<uint32_t>(iterations, 8);
    for (uint32_t i = 0; i < warmup; ++i) {
        fn();
    }
    context->Flush();

    context->Begin(disjoint.Get());
    context->End(start_query.Get());
    for (uint32_t i = 0; i < iterations; ++i) {
        fn();
    }
    context->End(end_query.Get());
    context->End(disjoint.Get());
    context->Flush();

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint_data = {};
    UINT64 start_ticks = 0;
    UINT64 end_ticks = 0;
    if (!wait_query(context, disjoint.Get(), &disjoint_data, sizeof(disjoint_data)) ||
        !wait_query(context, start_query.Get(), &start_ticks, sizeof(start_ticks)) ||
        !wait_query(context, end_query.Get(), &end_ticks, sizeof(end_ticks))) {
        error = "Timed out waiting for GPU timestamp query";
        return false;
    }
    if (disjoint_data.Disjoint || disjoint_data.Frequency == 0 || end_ticks < start_ticks) {
        error = "GPU timestamp query was disjoint";
        return false;
    }
    avg_ms = (static_cast<double>(end_ticks - start_ticks) * 1000.0) /
             static_cast<double>(disjoint_data.Frequency) /
             static_cast<double>(iterations);
    return true;
}

void set_viewport(ID3D11DeviceContext* context, uint32_t width, uint32_t height) {
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
}

GpuBenchmarkResult run_gpu_benchmark(const AnalysisOverlayGpuBenchmarkOptions& options,
                                     const std::vector<GpuRect>& rects) {
    GpuBenchmarkResult result;
    result.cu_count = static_cast<uint32_t>(rects.size());
    result.gpu_rect_upload_bytes =
        static_cast<uint64_t>(rects.size()) * static_cast<uint64_t>(sizeof(GpuRect));
    if (rects.empty()) {
        result.error = "overlay frame has no CU records";
        return result;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &feature_level,
        &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            flags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &device,
            &feature_level,
            &context);
        if (FAILED(hr)) {
            result.error = "D3D11CreateDevice failed";
            return result;
        }
        result.device_type = "warp";
    } else {
        result.device_type = "hardware";
    }

    EmbeddedInclude includes({
        {"common.hlsl", kCommonHlsl, sizeof(kCommonHlsl) - 1},
        {"color_pipeline.hlsl", kColorPipelineHlsl, sizeof(kColorPipelineHlsl) - 1},
        {"sampling.hlsl", kSamplingHlsl, sizeof(kSamplingHlsl) - 1},
    });
    ShaderPair color_shader;
    ShaderPair mask_shader;
    ShaderPair invert_shader;
    std::string error;
    if (!compile_shader_pair(device.Get(), kAnalysisOverlayRectHlsl, includes, false, color_shader, error) ||
        !compile_shader_pair(device.Get(), kAnalysisOverlayMaskRectHlsl, includes, false, mask_shader, error) ||
        !compile_shader_pair(device.Get(), kAnalysisOverlayInvertHlsl, includes, true, invert_shader, error)) {
        result.error = error;
        return result;
    }

    D3D11_BUFFER_DESC rect_desc = {};
    rect_desc.ByteWidth = static_cast<UINT>(rects.size() * sizeof(GpuRect));
    rect_desc.Usage = D3D11_USAGE_DYNAMIC;
    rect_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    rect_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    rect_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    rect_desc.StructureByteStride = sizeof(GpuRect);
    ComPtr<ID3D11Buffer> rect_buffer;
    hr = device->CreateBuffer(&rect_desc, nullptr, &rect_buffer);
    if (FAILED(hr)) {
        result.error = "CreateBuffer(rects) failed";
        return result;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC rect_srv_desc = {};
    rect_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    rect_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    rect_srv_desc.Buffer.NumElements = static_cast<UINT>(rects.size());
    ComPtr<ID3D11ShaderResourceView> rect_srv;
    hr = device->CreateShaderResourceView(rect_buffer.Get(), &rect_srv_desc, &rect_srv);
    if (FAILED(hr)) {
        result.error = "CreateShaderResourceView(rects) failed";
        return result;
    }
    auto upload_rects = [&]() {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(rect_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, rects.data(), rects.size() * sizeof(GpuRect));
            context->Unmap(rect_buffer.Get(), 0);
        }
    };
    upload_rects();
    const auto upload_start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < options.iterations; ++i) {
        upload_rects();
    }
    result.rect_upload_cpu_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - upload_start).count() /
        static_cast<double>(options.iterations);

    D3D11_TEXTURE2D_DESC target_desc = {};
    target_desc.Width = options.width;
    target_desc.Height = options.height;
    target_desc.MipLevels = 1;
    target_desc.ArraySize = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.Usage = D3D11_USAGE_DEFAULT;
    target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target_texture;
    ComPtr<ID3D11RenderTargetView> target_rtv;
    hr = device->CreateTexture2D(&target_desc, nullptr, &target_texture);
    if (FAILED(hr) ||
        FAILED(device->CreateRenderTargetView(target_texture.Get(), nullptr, &target_rtv))) {
        result.error = "Create render target failed";
        return result;
    }

    D3D11_TEXTURE2D_DESC mask_desc = target_desc;
    mask_desc.Format = DXGI_FORMAT_R8_UNORM;
    mask_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> mask_texture;
    ComPtr<ID3D11RenderTargetView> mask_rtv;
    ComPtr<ID3D11ShaderResourceView> mask_srv;
    hr = device->CreateTexture2D(&mask_desc, nullptr, &mask_texture);
    if (FAILED(hr) ||
        FAILED(device->CreateRenderTargetView(mask_texture.Get(), nullptr, &mask_rtv)) ||
        FAILED(device->CreateShaderResourceView(mask_texture.Get(), nullptr, &mask_srv))) {
        result.error = "Create mask target failed";
        return result;
    }

    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.ByteWidth = static_cast<UINT>(((sizeof(ShaderConstants) + 15) / 16) * 16);
    cb_desc.Usage = D3D11_USAGE_DEFAULT;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constant_buffer;
    hr = device->CreateBuffer(&cb_desc, nullptr, &constant_buffer);
    if (FAILED(hr)) {
        result.error = "Create constant buffer failed";
        return result;
    }
    ShaderConstants cb = {};
    cb.mode = 0;
    cb.track_count = 1;
    cb.split_pos = 0.5f;
    cb.zoom_ratio = 1.0f;
    cb.canvas_width = static_cast<float>(options.width);
    cb.canvas_height = static_cast<float>(options.height);
    cb.order[0] = 0;
    cb.order[1] = 1;
    cb.order[2] = 2;
    cb.order[3] = 3;
    for (int i = 0; i < 4; ++i) {
        cb.video_aspect[i] = options.height > 0
            ? static_cast<float>(options.width) / static_cast<float>(options.height)
            : 1.0f;
        cb.nv12_uv_scale_x[i] = 1.0f;
        cb.nv12_uv_scale_y[i] = 1.0f;
        cb.track_scale[i] = 1.0f;
        cb.display_offset_x[i] = 0.0f;
        cb.display_offset_y[i] = 0.0f;
        cb.inv_display_size_x[i] = 1.0f;
        cb.inv_display_size_y[i] = 1.0f;
        cb.color_range[i] = 1;
        cb.color_matrix[i] = 2;
        cb.color_transfer[i] = 1;
        cb.color_primaries[i] = 2;
    }
    context->UpdateSubresource(constant_buffer.Get(), 0, nullptr, &cb, 0, 0);
    ID3D11Buffer* cb_ptr = constant_buffer.Get();
    context->VSSetConstantBuffers(0, 1, &cb_ptr);
    context->PSSetConstantBuffers(0, 1, &cb_ptr);

    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    hr = device->CreateSamplerState(&sampler_desc, &sampler);
    if (FAILED(hr)) {
        result.error = "Create sampler failed";
        return result;
    }

    D3D11_BLEND_DESC alpha_blend_desc = {};
    alpha_blend_desc.RenderTarget[0].BlendEnable = TRUE;
    alpha_blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    alpha_blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    alpha_blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    alpha_blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    alpha_blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    alpha_blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    alpha_blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> alpha_blend;
    hr = device->CreateBlendState(&alpha_blend_desc, &alpha_blend);
    if (FAILED(hr)) {
        result.error = "Create alpha blend failed";
        return result;
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
    ComPtr<ID3D11BlendState> invert_blend;
    hr = device->CreateBlendState(&invert_blend_desc, &invert_blend);
    if (FAILED(hr)) {
        result.error = "Create invert blend failed";
        return result;
    }

    struct Vertex {
        float x, y, u, v;
    };
    const Vertex quad[] = {
        {-1.0f, -1.0f, 0.0f, 1.0f},
        {-1.0f,  1.0f, 0.0f, 0.0f},
        { 1.0f, -1.0f, 1.0f, 1.0f},
        { 1.0f,  1.0f, 1.0f, 0.0f},
    };
    D3D11_BUFFER_DESC vb_desc = {};
    vb_desc.ByteWidth = sizeof(quad);
    vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vb_data = {};
    vb_data.pSysMem = quad;
    ComPtr<ID3D11Buffer> fullscreen_vb;
    hr = device->CreateBuffer(&vb_desc, &vb_data, &fullscreen_vb);
    if (FAILED(hr)) {
        result.error = "Create fullscreen vertex buffer failed";
        return result;
    }

    const float blend_factor[4] = {0, 0, 0, 0};
    auto draw_color = [&]() {
        ID3D11RenderTargetView* rtv = target_rtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        set_viewport(context.Get(), options.width, options.height);
        context->OMSetBlendState(alpha_blend.Get(), blend_factor, 0xffffffff);
        ID3D11Buffer* null_vb = nullptr;
        UINT zero = 0;
        context->IASetInputLayout(nullptr);
        context->IASetVertexBuffers(0, 1, &null_vb, &zero, &zero);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(color_shader.vs.Get(), nullptr, 0);
        context->PSSetShader(color_shader.ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv = rect_srv.Get();
        context->VSSetShaderResources(28, 1, &srv);
        context->DrawInstanced(4, static_cast<UINT>(rects.size()), 0, 0);
        ID3D11ShaderResourceView* null_srv = nullptr;
        context->VSSetShaderResources(28, 1, &null_srv);
    };

    auto draw_mask = [&]() {
        float clear[4] = {0, 0, 0, 0};
        context->ClearRenderTargetView(mask_rtv.Get(), clear);
        ID3D11RenderTargetView* rtv = mask_rtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        set_viewport(context.Get(), options.width, options.height);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ID3D11Buffer* null_vb = nullptr;
        UINT zero = 0;
        context->IASetInputLayout(nullptr);
        context->IASetVertexBuffers(0, 1, &null_vb, &zero, &zero);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(mask_shader.vs.Get(), nullptr, 0);
        context->PSSetShader(mask_shader.ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv = rect_srv.Get();
        context->VSSetShaderResources(28, 1, &srv);
        context->DrawInstanced(4, static_cast<UINT>(rects.size()), 0, 0);
        ID3D11ShaderResourceView* null_srv = nullptr;
        context->VSSetShaderResources(28, 1, &null_srv);
    };

    auto draw_invert = [&]() {
        ID3D11RenderTargetView* rtv = target_rtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        set_viewport(context.Get(), options.width, options.height);
        context->OMSetBlendState(invert_blend.Get(), blend_factor, 0xffffffff);
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        ID3D11Buffer* vb = fullscreen_vb.Get();
        context->IASetInputLayout(invert_shader.layout.Get());
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(invert_shader.vs.Get(), nullptr, 0);
        context->PSSetShader(invert_shader.ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv = mask_srv.Get();
        ID3D11SamplerState* sampler_ptr = sampler.Get();
        context->PSSetShaderResources(24, 1, &srv);
        context->PSSetSamplers(0, 1, &sampler_ptr);
        context->Draw(4, 0);
        ID3D11ShaderResourceView* null_srvs[4] = {};
        context->PSSetShaderResources(24, 4, null_srvs);
    };

    auto draw_full = [&]() {
        if (options.with_grid) {
            draw_mask();
        }
        draw_color();
        if (options.with_grid) {
            draw_invert();
        }
    };

    if (!measure_gpu_ms(device.Get(), context.Get(), options.iterations, draw_color,
                        result.color_pass_gpu_ms, error)) {
        result.error = error;
        return result;
    }
    if (options.with_grid &&
        !measure_gpu_ms(device.Get(), context.Get(), options.iterations, draw_mask,
                        result.mask_pass_gpu_ms, error)) {
        result.error = error;
        return result;
    }
    if (options.with_grid) {
        draw_mask();
        if (!measure_gpu_ms(device.Get(), context.Get(), options.iterations, draw_invert,
                            result.invert_pass_gpu_ms, error)) {
            result.error = error;
            return result;
        }
    }
    if (!measure_gpu_ms(device.Get(), context.Get(), options.iterations, draw_full,
                        result.full_overlay_gpu_ms, error)) {
        result.error = error;
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace

int benchmark_analysis_overlay_gpu(const AnalysisOverlayGpuBenchmarkOptions& options) {
    if (options.frame == UINT32_MAX) {
        std::cerr << "Missing --frame N\n";
        return 1;
    }
    if (options.width == 0 || options.height == 0 || options.iterations == 0) {
        std::cerr << "--width, --height, and --iterations must be positive\n";
        return 1;
    }

    analysis::VachunkFile chunk;
    if (!chunk.open(options.path)) {
        std::cerr << "Failed to open VACHUNK: " << options.path << "\n";
        return 2;
    }
    analysis::VachunkOverlayFrameData frame;
    if (!analysis::read_overlay_vachunk_frame(chunk, options.frame, frame)) {
        std::cerr << "Failed to read overlay frame " << options.frame << "\n";
        return 2;
    }
    const std::vector<GpuRect> rects = build_rects(frame, options.width, options.height, options.mode);
    GpuBenchmarkResult result = run_gpu_benchmark(options, rects);
    if (!result.ok) {
        if (options.json) {
            std::cout << "{"
                      << "\"ok\":false,"
                      << "\"type\":\"overlayGpuBenchmark\","
                      << "\"path\":\"" << json_escape(options.path) << "\","
                      << "\"error\":\"" << json_escape(result.error) << "\""
                      << "}\n";
        } else {
            std::cerr << "Overlay GPU benchmark failed: " << result.error << "\n";
        }
        return 2;
    }

    if (options.json) {
        std::cout << "{"
                  << "\"ok\":true,"
                  << "\"type\":\"overlayGpuBenchmark\","
                  << "\"path\":\"" << json_escape(options.path) << "\","
                  << "\"frame\":" << options.frame << ","
                  << "\"mode\":\"" << json_escape(options.mode) << "\","
                  << "\"width\":" << options.width << ","
                  << "\"height\":" << options.height << ","
                  << "\"iterations\":" << options.iterations << ","
                  << "\"withGrid\":" << (options.with_grid ? "true" : "false") << ","
                  << "\"deviceType\":\"" << json_escape(result.device_type) << "\","
                  << "\"cuCount\":" << result.cu_count << ","
                  << "\"gpuRectUploadBytes\":" << result.gpu_rect_upload_bytes << ","
                  << "\"rectUploadCpuMs\":" << result.rect_upload_cpu_ms << ","
                  << "\"colorPassGpuMs\":" << result.color_pass_gpu_ms << ","
                  << "\"maskPassGpuMs\":" << result.mask_pass_gpu_ms << ","
                  << "\"invertPassGpuMs\":" << result.invert_pass_gpu_ms << ","
                  << "\"fullOverlayGpuMs\":" << result.full_overlay_gpu_ms
                  << "}\n";
    } else {
        std::cout << "Overlay GPU benchmark: frame=" << options.frame
                  << " mode=" << options.mode
                  << " iterations=" << options.iterations
                  << " grid=" << (options.with_grid ? "yes" : "no")
                  << " device=" << result.device_type
                  << " cus=" << result.cu_count
                  << " rect_upload_cpu=" << result.rect_upload_cpu_ms << "ms"
                  << " color_gpu=" << result.color_pass_gpu_ms << "ms"
                  << " mask_gpu=" << result.mask_pass_gpu_ms << "ms"
                  << " invert_gpu=" << result.invert_pass_gpu_ms << "ms"
                  << " full_gpu=" << result.full_overlay_gpu_ms << "ms\n";
    }
    return 0;
}

} // namespace vr::tools
