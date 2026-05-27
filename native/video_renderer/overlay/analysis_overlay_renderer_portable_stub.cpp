#include "video_renderer/overlay/analysis_overlay_renderer.h"

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
    const D3D11RenderResources&) {
    return {};
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

void AnalysisOverlayRenderer::draw(const PresentDecision&,
                                   const RendererDrawTrackSnapshotList&,
                                   D3D11Device&,
                                   D3D11RenderResources&,
                                   int,
                                   int) {}

bool AnalysisOverlayRenderer::composite_bgra(const RendererDrawSnapshot&,
                                             uint8_t*,
                                             int,
                                             int,
                                             size_t) {
    return false;
}

} // namespace vr
