#include "video_renderer/overlay/analysis_overlay_renderer.h"

#include "analysis/analysis_manager.h"
#include "analysis/cache/overlay_raster.h"
#include "video_renderer/d3d11/memory_estimate.h"
#include "video_renderer/d3d11/render_backend.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

namespace vr {

uint32_t pack_overlay_bgra(analysis::OverlayColor color) {
    return static_cast<uint32_t>(color.b) |
           (static_cast<uint32_t>(color.g) << 8) |
           (static_cast<uint32_t>(color.r) << 16) |
           (static_cast<uint32_t>(color.a) << 24);
}

uint32_t pack_overlay_track_payload(int slot, uint8_t line_alpha) {
    return static_cast<uint32_t>(slot & 0xff) |
           (static_cast<uint32_t>(line_alpha) << 8);
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

AnalysisOverlayMemoryStats snapshot_analysis_overlay_memory_stats(
    const D3D11RenderResources& resources) {
    AnalysisOverlayMemoryStats stats;

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (resources.overlay_rect_capacity[i] > 0) {
            stats.estimated_bytes +=
                static_cast<uint64_t>(resources.overlay_rect_capacity[i]) *
                static_cast<uint64_t>(AnalysisOverlayRenderer::gpu_rect_size());
        }
        if (resources.overlay_textures[i]) {
            D3D11_TEXTURE2D_DESC desc = {};
            resources.overlay_textures[i]->GetDesc(&desc);
            stats.estimated_bytes += estimate_d3d11_texture_bytes(desc);
            stats.width = std::max(stats.width, static_cast<int>(desc.Width));
            stats.height = std::max(stats.height, static_cast<int>(desc.Height));
        }
        if (resources.overlay_mask_textures[i]) {
            D3D11_TEXTURE2D_DESC mask_desc = {};
            resources.overlay_mask_textures[i]->GetDesc(&mask_desc);
            stats.estimated_bytes += estimate_d3d11_texture_bytes(mask_desc);
            stats.width = std::max(stats.width, static_cast<int>(mask_desc.Width));
            stats.height = std::max(stats.height, static_cast<int>(mask_desc.Height));
        }
    }

    return stats;
}

void AnalysisOverlayRenderer::reset() {
    for (auto& pixels : overlay_pixels_) {
        pixels.clear();
    }
    for (auto& rects : overlay_rects_) {
        rects.clear();
    }
    for (auto& cache : overlay_cache_) {
        cache = {};
    }
}

bool AnalysisOverlayRenderer::ensure_overlay_texture(D3D11Device& device,
                                                     D3D11RenderResources& resources,
                                                     int slot,
                                                     int width,
                                                     int height,
                                                     bool need_color,
                                                     bool need_mask) {
    if (!device.device() ||
        slot < 0 || slot >= static_cast<int>(kMaxTracks) ||
        width <= 0 || height <= 0) {
        return false;
    }
    if (!need_color && !need_mask) {
        return true;
    }

    const bool color_size_matches =
        resources.overlay_width[slot] == width &&
        resources.overlay_height[slot] == height;
    const bool mask_size_matches =
        resources.overlay_mask_width[slot] == width &&
        resources.overlay_mask_height[slot] == height;
    const bool has_color =
        resources.overlay_textures[slot] &&
        resources.overlay_srvs[slot];
    const bool has_mask =
        resources.overlay_mask_textures[slot] &&
        resources.overlay_mask_srvs[slot] &&
        resources.overlay_mask_rtvs[slot];
    if ((!need_color || (color_size_matches && has_color)) &&
        (!need_mask || (mask_size_matches && has_mask))) {
        return true;
    }

    if (need_color && !color_size_matches) {
        resources.overlay_textures[slot].Reset();
        resources.overlay_srvs[slot].Reset();
        resources.overlay_width[slot] = 0;
        resources.overlay_height[slot] = 0;
    }
    if (need_mask && !mask_size_matches) {
        resources.overlay_mask_textures[slot].Reset();
        resources.overlay_mask_srvs[slot].Reset();
        resources.overlay_mask_rtvs[slot].Reset();
        resources.overlay_mask_width[slot] = 0;
        resources.overlay_mask_height[slot] = 0;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = S_OK;
    if (need_color && (!color_size_matches || !has_color)) {
        resources.overlay_textures[slot].Reset();
        resources.overlay_srvs[slot].Reset();
        hr = device.device()->CreateTexture2D(
            &desc, nullptr, &resources.overlay_textures[slot]);
        if (FAILED(hr) || !resources.overlay_textures[slot]) {
            spdlog::error("[Renderer] CreateTexture2D(analysis overlay) failed: HRESULT {:#x}",
                          static_cast<unsigned long>(hr));
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        hr = device.device()->CreateShaderResourceView(
            resources.overlay_textures[slot].Get(), &srv_desc, &resources.overlay_srvs[slot]);
        if (FAILED(hr) || !resources.overlay_srvs[slot]) {
            spdlog::error("[Renderer] CreateShaderResourceView(analysis overlay) failed: HRESULT {:#x}",
                          static_cast<unsigned long>(hr));
            resources.overlay_textures[slot].Reset();
            return false;
        }
    }

    if (need_mask && (!mask_size_matches || !has_mask)) {
        resources.overlay_mask_textures[slot].Reset();
        resources.overlay_mask_srvs[slot].Reset();
        D3D11_TEXTURE2D_DESC mask_desc = {};
        mask_desc.Width = static_cast<UINT>(width);
        mask_desc.Height = static_cast<UINT>(height);
        mask_desc.MipLevels = 1;
        mask_desc.ArraySize = 1;
        mask_desc.Format = DXGI_FORMAT_R8_UNORM;
        mask_desc.SampleDesc.Count = 1;
        mask_desc.SampleDesc.Quality = 0;
        mask_desc.Usage = D3D11_USAGE_DEFAULT;
        mask_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        mask_desc.CPUAccessFlags = 0;
        hr = device.device()->CreateTexture2D(
            &mask_desc, nullptr, &resources.overlay_mask_textures[slot]);
        if (FAILED(hr) || !resources.overlay_mask_textures[slot]) {
            spdlog::error("[Renderer] CreateTexture2D(analysis overlay mask) failed: HRESULT {:#x}",
                          static_cast<unsigned long>(hr));
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC mask_srv_desc = {};
        mask_srv_desc.Format = mask_desc.Format;
        mask_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        mask_srv_desc.Texture2D.MipLevels = 1;
        hr = device.device()->CreateShaderResourceView(
            resources.overlay_mask_textures[slot].Get(), &mask_srv_desc, &resources.overlay_mask_srvs[slot]);
        if (FAILED(hr) || !resources.overlay_mask_srvs[slot]) {
            spdlog::error("[Renderer] CreateShaderResourceView(analysis overlay mask) failed: HRESULT {:#x}",
                          static_cast<unsigned long>(hr));
            resources.overlay_mask_textures[slot].Reset();
            return false;
        }

        D3D11_RENDER_TARGET_VIEW_DESC mask_rtv_desc = {};
        mask_rtv_desc.Format = mask_desc.Format;
        mask_rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        mask_rtv_desc.Texture2D.MipSlice = 0;
        hr = device.device()->CreateRenderTargetView(
            resources.overlay_mask_textures[slot].Get(),
            &mask_rtv_desc,
            &resources.overlay_mask_rtvs[slot]);
        if (FAILED(hr) || !resources.overlay_mask_rtvs[slot]) {
            spdlog::error("[Renderer] CreateRenderTargetView(analysis overlay mask) failed: HRESULT {:#x}",
                          static_cast<unsigned long>(hr));
            resources.overlay_mask_textures[slot].Reset();
            resources.overlay_mask_srvs[slot].Reset();
            return false;
        }
    }

    if (need_color) {
        resources.overlay_width[slot] = width;
        resources.overlay_height[slot] = height;
    }
    if (need_mask) {
        resources.overlay_mask_width[slot] = width;
        resources.overlay_mask_height[slot] = height;
    }
    return true;
}

bool AnalysisOverlayRenderer::ensure_overlay_rect_buffer(D3D11Device& device,
                                                         D3D11RenderResources& resources,
                                                         int slot,
                                                         uint32_t rect_count) {
    if (!device.device() ||
        slot < 0 || slot >= static_cast<int>(kMaxTracks)) {
        return false;
    }
    if (rect_count == 0) {
        return true;
    }

    if (resources.overlay_rect_buffers[slot] &&
        resources.overlay_rect_srvs[slot] &&
        resources.overlay_rect_capacity[slot] >= rect_count) {
        return true;
    }

    uint32_t capacity = std::max<uint32_t>(rect_count, 1024);
    if (resources.overlay_rect_capacity[slot] > 0) {
        capacity = std::max<uint32_t>(
            capacity,
            resources.overlay_rect_capacity[slot] * 2);
    }

    resources.overlay_rect_buffers[slot].Reset();
    resources.overlay_rect_srvs[slot].Reset();
    resources.overlay_rect_capacity[slot] = 0;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = capacity * static_cast<UINT>(sizeof(AnalysisOverlayGpuRect));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = static_cast<UINT>(sizeof(AnalysisOverlayGpuRect));

    HRESULT hr = device.device()->CreateBuffer(
        &desc, nullptr, &resources.overlay_rect_buffers[slot]);
    if (FAILED(hr) || !resources.overlay_rect_buffers[slot]) {
        spdlog::error("[Renderer] CreateBuffer(analysis overlay rects) failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srv_desc.Buffer.FirstElement = 0;
    srv_desc.Buffer.NumElements = capacity;
    hr = device.device()->CreateShaderResourceView(
        resources.overlay_rect_buffers[slot].Get(),
        &srv_desc,
        &resources.overlay_rect_srvs[slot]);
    if (FAILED(hr) || !resources.overlay_rect_srvs[slot]) {
        spdlog::error("[Renderer] CreateShaderResourceView(analysis overlay rects) failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        resources.overlay_rect_buffers[slot].Reset();
        return false;
    }

    resources.overlay_rect_capacity[slot] = capacity;
    return true;
}

bool AnalysisOverlayRenderer::render_overlay_mask(D3D11Device& device,
                                                  D3D11RenderResources& resources,
                                                  int slot,
                                                  uint32_t rect_count,
                                                  int target_width,
                                                  int target_height) {
    if (!device.context() ||
        slot < 0 || slot >= static_cast<int>(kMaxTracks) ||
        rect_count == 0 ||
        target_width <= 0 || target_height <= 0) {
        return false;
    }
    if (!resources.overlay_mask_rtvs[slot] ||
        !resources.overlay_rect_srvs[slot] ||
        !resources.cached_rtv ||
        !resources.overlay_mask_rect_shader.vs ||
        !resources.overlay_mask_rect_shader.ps) {
        return false;
    }

    auto* ctx = device.context();
    ID3D11ShaderResourceView* null_masks[4] = {};
    ID3D11ShaderResourceView* null_rect_srv = nullptr;

    ctx->PSSetShaderResources(24, 4, null_masks);
    ctx->VSSetShaderResources(28, 1, &null_rect_srv);

    float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ID3D11RenderTargetView* mask_rtv = resources.overlay_mask_rtvs[slot].Get();
    ctx->ClearRenderTargetView(mask_rtv, clear);
    ctx->OMSetRenderTargets(1, &mask_rtv, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);

    D3D11_VIEWPORT mask_vp = {};
    mask_vp.Width = static_cast<float>(target_width);
    mask_vp.Height = static_cast<float>(target_height);
    mask_vp.MinDepth = 0.0f;
    mask_vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &mask_vp);

    ID3D11Buffer* null_vb = nullptr;
    UINT zero = 0;
    ctx->IASetInputLayout(nullptr);
    ctx->IASetVertexBuffers(0, 1, &null_vb, &zero, &zero);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(resources.overlay_mask_rect_shader.vs.Get(), nullptr, 0);
    ctx->PSSetShader(resources.overlay_mask_rect_shader.ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* rect_srv = resources.overlay_rect_srvs[slot].Get();
    ctx->VSSetShaderResources(28, 1, &rect_srv);
    ctx->DrawInstanced(4, rect_count, 0, 0);
    ctx->VSSetShaderResources(28, 1, &null_rect_srv);

    ID3D11RenderTargetView* target_rtv = resources.cached_rtv.Get();
    ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
    D3D11_VIEWPORT target_vp = {};
    target_vp.Width = static_cast<float>(target_width);
    target_vp.Height = static_cast<float>(target_height);
    target_vp.MinDepth = 0.0f;
    target_vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &target_vp);
    return true;
}

void AnalysisOverlayRenderer::draw(const PresentDecision& decision,
                                   const RendererDrawTrackSnapshotList& tracks,
                                   D3D11Device& device,
                                   D3D11RenderResources& resources,
                                   int target_width,
                                   int target_height) {
    if (!device.context()) return;
    if (!resources.overlay_shader.vs ||
        !resources.overlay_shader.ps ||
        !resources.overlay_invert_shader.vs ||
        !resources.overlay_invert_shader.ps ||
        !resources.overlay_contrast_shader.vs ||
        !resources.overlay_contrast_shader.ps ||
        !resources.overlay_mask_rect_shader.vs ||
        !resources.overlay_mask_rect_shader.ps ||
        !resources.overlay_blend_state ||
        !resources.overlay_invert_blend_state ||
        !resources.overlay_sampler_state) {
        return;
    }

    auto& manager = analysis::AnalysisManager::instance();
    const auto& overlay = manager.overlay_state();
    const bool show_grid = overlay.show_cu_grid.load(std::memory_order_acquire);
    const bool show_qp = overlay.show_qp_heatmap.load(std::memory_order_acquire);
    const bool show_pred = overlay.show_pred_mode.load(std::memory_order_acquire);
    const bool show_lines = overlay.show_pred_lines.load(std::memory_order_acquire);
    const bool show_bit_cost = overlay.show_cu_bit_cost_heatmap.load(std::memory_order_acquire);
    const int mode = overlay.mode.load(std::memory_order_acquire);
    const int file_id = overlay.track_file_id.load(std::memory_order_acquire);
    const int opacity_permille =
        std::clamp(overlay.opacity_permille.load(std::memory_order_acquire), 0, 1000);

    if (!show_grid && !show_qp && !show_pred && !show_lines && !show_bit_cost &&
        mode != 0 && mode != 1 && mode != 2 && mode != 3 && mode != 4) {
        return;
    }

    auto overlay_tracks = manager.overlay_track_snapshot();
    if (overlay_tracks.empty() && manager.is_loaded() && file_id >= 0) {
        overlay_tracks.emplace_back(file_id, manager.session_snapshot());
    }
    if (overlay_tracks.empty()) {
        return;
    }

    const uint8_t base_alpha = static_cast<uint8_t>(std::clamp(
        opacity_permille * 255 / 1000, 0, 255));
    const uint8_t line_alpha = base_alpha;
    // Current Dart primary overlay modes are: 0=CU, 1=QP heatmap,
    // 2=bitrate/bit-cost heatmap. Keep 3/4 as compatibility with the
    // previous five-mode UI.
    const bool qp_primary = show_qp || mode == 1 || mode == 3;
    const bool bit_cost_primary = show_bit_cost || mode == 2 || mode == 4;
    const bool pred_primary = show_pred;
    const bool line_primary = show_lines;
    const bool heatmap_primary = qp_primary || bit_cost_primary;
    const uint8_t fill_alpha = heatmap_primary
        ? base_alpha
        : static_cast<uint8_t>(base_alpha * 2 / 5);

    auto* ctx = device.context();
    auto bind_overlay_target = [&]() -> bool {
        if (!resources.cached_rtv || target_width <= 0 || target_height <= 0) {
            return false;
        }
        ID3D11RenderTargetView* target_rtv = resources.cached_rtv.Get();
        ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
        D3D11_VIEWPORT target_vp = {};
        target_vp.Width = static_cast<float>(target_width);
        target_vp.Height = static_cast<float>(target_height);
        target_vp.MinDepth = 0.0f;
        target_vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &target_vp);
        return true;
    };
    if (!bind_overlay_target()) {
        return;
    }

    auto upload_overlay = [&](ID3D11Texture2D* texture,
                              const std::vector<uint8_t>& pixels,
                              int upload_height,
                              size_t row_bytes,
                              const char* label) -> bool {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = ctx->Map(
            texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            spdlog::warn("[Renderer] Map({}) failed: HRESULT {:#x}",
                         label,
                         static_cast<unsigned long>(hr));
            return false;
        }
        const uint8_t* src = pixels.data();
        auto* dst = static_cast<uint8_t*>(mapped.pData);
        for (int y = 0; y < upload_height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
                        src + static_cast<size_t>(y) * row_bytes,
                        row_bytes);
        }
        ctx->Unmap(texture, 0);
        return true;
    };
    auto upload_rects = [&](int slot,
                            const std::vector<AnalysisOverlayGpuRect>& rects) -> bool {
        if (rects.empty()) {
            return true;
        }
        if (!ensure_overlay_rect_buffer(
                device, resources, slot, static_cast<uint32_t>(rects.size()))) {
            return false;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = ctx->Map(
            resources.overlay_rect_buffers[slot].Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped);
        if (FAILED(hr)) {
            spdlog::warn("[Renderer] Map(analysis overlay rects) failed: HRESULT {:#x}",
                         static_cast<unsigned long>(hr));
            return false;
        }
        std::memcpy(
            mapped.pData,
            rects.data(),
            rects.size() * sizeof(AnalysisOverlayGpuRect));
        ctx->Unmap(resources.overlay_rect_buffers[slot].Get(), 0);
        return true;
    };

    std::array<ID3D11ShaderResourceView*, kMaxTracks> color_srvs = {};
    std::array<ID3D11ShaderResourceView*, kMaxTracks> mask_srvs = {};
    std::array<ID3D11ShaderResourceView*, kMaxTracks> rect_srvs = {};
    std::array<uint32_t, kMaxTracks> rect_counts = {};
    bool has_color_overlay = false;
    bool has_color_instances = false;
    bool has_line_mask = false;

    auto prepare_track_overlay = [&](int track_file_id,
                                     const analysis::AnalysisSession& track_analysis) {
        int slot = -1;
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].active && tracks[i].file_id == track_file_id) {
                slot = static_cast<int>(i);
                break;
            }
        }
        if (slot < 0 || slot >= static_cast<int>(kMaxTracks)) {
            return;
        }
        const auto& track = tracks[slot];

        const int frame_idx = track_analysis.current_frame_idx(
            decision.frames[slot].has_value()
                ? decision.frames[slot]->pts_us
                : std::max<int64_t>(0, decision.current_pts_us - track.offset_us));
        if (frame_idx < 0 || frame_idx >= track_analysis.frame_count()) {
            return;
        }

        const int video_w = static_cast<int>(track_analysis.video_width());
        const int video_h = static_cast<int>(track_analysis.video_height());
        if (video_w <= 0 || video_h <= 0) {
            return;
        }
        const bool needs_line_instances =
            line_alpha > 0 && (show_grid || mode == 0 || pred_primary);
        const bool needs_color_texture = line_primary && line_alpha > 0;
        const bool texture_was_valid =
            !needs_color_texture ||
            (resources.overlay_width[slot] == video_w &&
             resources.overlay_height[slot] == video_h &&
             resources.overlay_textures[slot]);
        if (!ensure_overlay_texture(
                device, resources, slot, video_w, video_h, needs_color_texture, false)) {
            return;
        }
        if (needs_line_instances &&
            !ensure_overlay_texture(
                device, resources, slot, target_width, target_height, false, true)) {
            return;
        }

        auto& cache = overlay_cache_[slot];
        const bool dirty =
            !texture_was_valid ||
            !cache.valid ||
            cache.track_file_id != track_file_id ||
            cache.frame_index != frame_idx ||
            cache.mode != mode ||
            cache.opacity_permille != opacity_permille ||
            cache.width != video_w ||
            cache.height != video_h ||
            cache.show_grid != show_grid ||
            cache.show_qp != show_qp ||
            cache.show_pred != show_pred ||
            cache.show_lines != show_lines ||
            cache.show_bit_cost != show_bit_cost;

        if (dirty) {
            auto frame = track_analysis.read_overlay_frame(frame_idx);
            cache = {};
            cache.track_file_id = track_file_id;
            cache.frame_index = frame_idx;
            cache.mode = mode;
            cache.opacity_permille = opacity_permille;
            cache.width = video_w;
            cache.height = video_h;
            cache.show_grid = show_grid;
            cache.show_qp = show_qp;
            cache.show_pred = show_pred;
            cache.show_lines = show_lines;
            cache.show_bit_cost = show_bit_cost;

            if (frame.cus.empty()) {
                return;
            }

            auto& color_pixels = overlay_pixels_[slot];
            auto& rects = overlay_rects_[slot];
            rects.clear();
            if (needs_color_texture) {
                const size_t pixel_count =
                    static_cast<size_t>(video_w) * static_cast<size_t>(video_h);
                const size_t color_bytes = pixel_count * 4;
                if (color_pixels.size() != color_bytes) {
                    color_pixels.resize(color_bytes);
                }
                std::fill(color_pixels.begin(), color_pixels.end(), 0);
            }
            rects.reserve(frame.cus.size());

            auto push_rect = [&](int x0,
                                 int y0,
                                 int x1,
                                 int y1,
                                 analysis::OverlayColor color,
                                 bool color_instance) {
                AnalysisOverlayGpuRect rect = {};
                rect.rect_uv0 = pack_overlay_uv16(x0, video_w, y0, video_h);
                rect.rect_uv1 = pack_overlay_uv16(x1, video_w, y1, video_h);
                rect.color_bgra = pack_overlay_bgra(color);
                rect.track_idx = pack_overlay_track_payload(slot, line_alpha);
                rects.push_back(rect);
                if (color_instance) {
                    cache.has_color_instances = true;
                }
            };

            for (const auto& cu : frame.cus) {
                const auto& c = cu.common;
                const int x0 = std::clamp(static_cast<int>(c.x), 0, video_w);
                const int y0 = std::clamp(static_cast<int>(c.y), 0, video_h);
                const int x1 = std::clamp(static_cast<int>(c.x + c.w), 0, video_w);
                const int y1 = std::clamp(static_cast<int>(c.y + c.h), 0, video_h);
                if (x1 <= x0 || y1 <= y0) continue;

                if (bit_cost_primary) {
                    if (fill_alpha > 0) {
                        push_rect(
                            x0, y0, x1, y1,
                            analysis::cu_bit_density_color(c, fill_alpha),
                            true);
                    }
                } else if (qp_primary) {
                    if (fill_alpha > 0) {
                        push_rect(
                            x0, y0, x1, y1, analysis::qp_color(c.qp, fill_alpha), true);
                    }
                } else if (pred_primary) {
                    if (fill_alpha > 0) {
                        push_rect(
                            x0, y0, x1, y1,
                            c.pred_mode == 1
                                ? analysis::OverlayColor{
                                      80, 235, 90, static_cast<uint8_t>(fill_alpha * 3 / 4)}
                            : analysis::pred_color(
                                  c.pred_mode, cu.inter,
                                  static_cast<uint8_t>(fill_alpha * 3 / 4)),
                            true);
                    }
                }

                if (needs_line_instances) {
                    if (!cache.has_color_instances) {
                        push_rect(
                            x0, y0, x1, y1,
                            analysis::OverlayColor{0, 0, 0, 0},
                            false);
                    }
                    cache.has_mask = true;
                }

                if (needs_color_texture && c.pred_mode != 1) {
                    const int cx = (x0 + x1) / 2;
                    const int cy = (y0 + y1) / 2;
                    const int dx = std::clamp(static_cast<int>(cu.inter.mv_l0_x / 16), -80, 80);
                    const int dy = std::clamp(static_cast<int>(cu.inter.mv_l0_y / 16), -80, 80);
                    analysis::draw_overlay_line(
                        color_pixels, video_w, video_h, cx, cy, cx + dx, cy + dy,
                        analysis::OverlayColor{80, 180, 255, line_alpha});
                    cache.has_color = true;
                }
            }

            if (!rects.empty()) {
                if (!upload_rects(slot, rects)) {
                    cache = {};
                    return;
                }
                if (cache.has_color_instances) {
                    cache.color_instance_count = static_cast<uint32_t>(rects.size());
                }
                if (cache.has_mask) {
                    cache.mask_instance_count = static_cast<uint32_t>(rects.size());
                }
            }
            if (cache.has_color) {
                if (!upload_overlay(
                        resources.overlay_textures[slot].Get(),
                        color_pixels,
                        video_h,
                        static_cast<size_t>(video_w) * 4,
                        "analysis overlay")) {
                    cache = {};
                    return;
                }
            }
            cache.valid = true;
        }

        if (cache.valid && cache.has_color) {
            color_srvs[slot] = resources.overlay_srvs[slot].Get();
            has_color_overlay = true;
        }
        if (cache.valid &&
            cache.has_color_instances &&
            cache.color_instance_count > 0 &&
            resources.overlay_rect_srvs[slot]) {
            rect_srvs[slot] = resources.overlay_rect_srvs[slot].Get();
            rect_counts[slot] = cache.color_instance_count;
            has_color_instances = true;
        }
        if (cache.valid && cache.has_mask) {
            if (!render_overlay_mask(
                    device,
                    resources,
                    slot,
                    cache.mask_instance_count,
                    target_width,
                    target_height)) {
                return;
            }
            mask_srvs[slot] = resources.overlay_mask_srvs[slot].Get();
            has_line_mask = true;
        }
    };

    for (const auto& [track_file_id, track_analysis] : overlay_tracks) {
        if (track_analysis) {
            prepare_track_overlay(track_file_id, *track_analysis);
        }
    }

    if (!has_color_overlay && !has_color_instances && !has_line_mask) {
        return;
    }

    ID3D11SamplerState* sampler = resources.overlay_sampler_state.Get();
    ID3D11ShaderResourceView* null_srvs[4] = {};
    float blend_factor[4] = {0, 0, 0, 0};

    if (has_color_instances) {
        ID3D11Buffer* null_vb = nullptr;
        UINT zero = 0;
        ID3D11ShaderResourceView* null_rect_srv = nullptr;
        ctx->OMSetBlendState(resources.overlay_blend_state.Get(), blend_factor, 0xffffffff);
        ctx->VSSetShader(resources.overlay_rect_shader.vs.Get(), nullptr, 0);
        ctx->PSSetShader(resources.overlay_rect_shader.ps.Get(), nullptr, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetVertexBuffers(0, 1, &null_vb, &zero, &zero);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        for (size_t slot = 0; slot < kMaxTracks; ++slot) {
            if (!rect_srvs[slot] || rect_counts[slot] == 0) {
                continue;
            }
            ID3D11ShaderResourceView* srv = rect_srvs[slot];
            ctx->VSSetShaderResources(28, 1, &srv);
            ctx->DrawInstanced(4, rect_counts[slot], 0, 0);
        }
        ctx->VSSetShaderResources(28, 1, &null_rect_srv);
    }

    auto bind_fullscreen_quad = [&]() {
        UINT stride = sizeof(float) * 4;
        UINT offset = 0;
        ID3D11Buffer* vb = resources.vertex_buffer.Get();
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    };

    if (has_color_overlay) {
        bind_fullscreen_quad();
        ctx->OMSetBlendState(resources.overlay_blend_state.Get(), blend_factor, 0xffffffff);
        ctx->VSSetShader(resources.overlay_shader.vs.Get(), nullptr, 0);
        ctx->PSSetShader(resources.overlay_shader.ps.Get(), nullptr, 0);
        if (resources.overlay_shader.layout) {
            ctx->IASetInputLayout(resources.overlay_shader.layout.Get());
        }
        ctx->PSSetShaderResources(20, static_cast<UINT>(color_srvs.size()), color_srvs.data());
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->Draw(4, 0);
        ctx->PSSetShaderResources(20, 4, null_srvs);
    }

    if (has_line_mask) {
        bind_fullscreen_quad();
        ctx->OMSetBlendState(
            resources.overlay_invert_blend_state.Get(), blend_factor, 0xffffffff);
        ctx->VSSetShader(resources.overlay_invert_shader.vs.Get(), nullptr, 0);
        ctx->PSSetShader(resources.overlay_invert_shader.ps.Get(), nullptr, 0);
        if (resources.overlay_invert_shader.layout) {
            ctx->IASetInputLayout(resources.overlay_invert_shader.layout.Get());
        }
        ctx->PSSetShaderResources(24, static_cast<UINT>(mask_srvs.size()), mask_srvs.data());
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->Draw(4, 0);

        ctx->OMSetBlendState(resources.overlay_blend_state.Get(), blend_factor, 0xffffffff);
        ctx->VSSetShader(resources.overlay_contrast_shader.vs.Get(), nullptr, 0);
        ctx->PSSetShader(resources.overlay_contrast_shader.ps.Get(), nullptr, 0);
        if (resources.overlay_contrast_shader.layout) {
            ctx->IASetInputLayout(resources.overlay_contrast_shader.layout.Get());
        }
        ctx->PSSetShaderResources(24, static_cast<UINT>(mask_srvs.size()), mask_srvs.data());
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->Draw(4, 0);
        ctx->PSSetShaderResources(24, 4, null_srvs);
    }

    ID3D11ShaderResourceView* null_rect_srv = nullptr;
    ctx->VSSetShaderResources(28, 1, &null_rect_srv);
    ctx->PSSetShaderResources(20, 4, null_srvs);
    ctx->PSSetShaderResources(24, 4, null_srvs);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    (void)bind_overlay_target();
}

} // namespace vr
