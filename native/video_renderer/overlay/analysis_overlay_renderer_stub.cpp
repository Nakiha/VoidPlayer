#include "video_renderer/overlay/analysis_overlay_renderer.h"

#include "windows/d3d11/memory_estimate.h"
#include "windows/d3d11/render_backend.h"

#include <algorithm>
#include <cmath>

namespace vr {

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

void AnalysisOverlayRenderer::draw(const RendererDrawSnapshot& snapshot,
                                   D3D11Device& device,
                                   D3D11RenderResources& resources,
                                   int target_width,
                                   int target_height) {
    (void)snapshot;
    (void)device;
    (void)resources;
    (void)target_width;
    (void)target_height;
}

bool AnalysisOverlayRenderer::composite_bgra(const RendererDrawSnapshot&,
                                             uint8_t*,
                                             int,
                                             int,
                                             size_t) {
    return false;
}

} // namespace vr
