#pragma once

#include "video_renderer/render/renderer_draw_snapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vr {

class D3D11Device;
struct D3D11RenderResources;
struct AnalysisOverlayMemoryStats {
    uint64_t estimated_bytes = 0;
    int width = 0;
    int height = 0;
};

uint32_t pack_overlay_uv16(int a, int a_extent, int b, int b_extent);

class AnalysisOverlayRenderer {
    struct AnalysisOverlayGpuRect {
        uint32_t rect_uv0 = 0;
        uint32_t rect_uv1 = 0;
        uint32_t color_bgra = 0;
        uint32_t track_idx = 0;
    };

public:
    static constexpr std::size_t gpu_rect_size() {
        return sizeof(AnalysisOverlayGpuRect);
    }

    void reset();
    void draw(const PresentDecision& decision,
              const RendererDrawTrackSnapshotList& tracks,
              D3D11Device& device,
              D3D11RenderResources& resources,
              int target_width,
              int target_height);
    bool composite_bgra(const RendererDrawSnapshot& snapshot,
                        uint8_t* target_bgra,
                        int target_width,
                        int target_height,
                        size_t target_stride_bytes);

private:
    struct AnalysisOverlayCache {
        bool valid = false;
        bool has_color = false;
        bool has_color_instances = false;
        bool has_mask = false;
        uint32_t color_instance_count = 0;
        uint32_t mask_instance_count = 0;
        int track_file_id = -1;
        int frame_index = -1;
        int mode = -1;
        int opacity_permille = -1;
        int width = 0;
        int height = 0;
        bool show_grid = false;
        bool show_qp = false;
        bool show_pred = false;
        bool show_lines = false;
        bool show_bit_cost = false;
    };

    bool ensure_overlay_texture(D3D11Device& device,
                                D3D11RenderResources& resources,
                                int slot,
                                int width,
                                int height,
                                bool need_color,
                                bool need_mask);
    bool ensure_overlay_rect_buffer(D3D11Device& device,
                                    D3D11RenderResources& resources,
                                    int slot,
                                    uint32_t rect_count);
    bool render_overlay_mask(D3D11Device& device,
                             D3D11RenderResources& resources,
                             int slot,
                             uint32_t rect_count,
                             int target_width,
                             int target_height);

    std::array<std::vector<uint8_t>, kMaxTracks> overlay_pixels_;
    std::array<std::vector<AnalysisOverlayGpuRect>, kMaxTracks> overlay_rects_;
    std::array<AnalysisOverlayCache, kMaxTracks> overlay_cache_;
};

AnalysisOverlayMemoryStats snapshot_analysis_overlay_memory_stats(
    const D3D11RenderResources& resources);

} // namespace vr
