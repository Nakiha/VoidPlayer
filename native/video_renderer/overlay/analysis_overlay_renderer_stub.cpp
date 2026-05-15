#include "video_renderer/overlay/analysis_overlay_renderer.h"

#include "video_renderer/d3d11/memory_estimate.h"
#include "video_renderer/d3d11/render_backend.h"

#include <algorithm>

namespace vr {

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

void AnalysisOverlayRenderer::draw(const PresentDecision& decision,
                                   const RendererDrawTrackSnapshotList& tracks,
                                   D3D11Device& device,
                                   D3D11RenderResources& resources,
                                   int target_width,
                                   int target_height) {
    (void)decision;
    (void)tracks;
    (void)device;
    (void)resources;
    (void)target_width;
    (void)target_height;
}

} // namespace vr
