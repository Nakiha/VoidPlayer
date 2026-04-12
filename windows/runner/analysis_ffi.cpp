#include "analysis_ffi.h"
#include "analysis/analysis_manager.h"

#include <cstring>
#include <algorithm>

// Callback registered by video_renderer_plugin to provide current PTS.
// Avoids analysis_ffi needing to know about vr::Renderer.
static int64_t (*g_get_current_pts_us)() = nullptr;

void naki_analysis_register_pts_callback(int64_t (*cb)()) {
    g_get_current_pts_us = cb;
}

// ---- dart:ffi analysis exports ----
static NakiAnalysisSummary g_analysis_summary = {};

extern "C" __declspec(dllexport)
int32_t naki_analysis_load(const char* vbs2_path, const char* vbi_path, const char* vbt_path) {
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return mgr.load(vbs2_path, vbi_path, vbt_path) ? 1 : 0;
}

extern "C" __declspec(dllexport)
void naki_analysis_unload() {
    vr::analysis::AnalysisManager::instance().unload();
}

extern "C" __declspec(dllexport)
const NakiAnalysisSummary* naki_analysis_get_summary() {
    auto& s = g_analysis_summary;
    std::memset(&s, 0, sizeof(s));

    auto& mgr = vr::analysis::AnalysisManager::instance();
    if (!mgr.is_loaded()) return &s;

    s.loaded = 1;
    const auto& vbs2 = mgr.vbs2();
    const auto& vbi = mgr.vbi();
    const auto& vbt = mgr.vbt();

    s.frame_count = vbs2.frame_count();
    s.packet_count = vbt.packet_count();
    s.nalu_count = vbi.nalu_count();
    s.video_width = vbs2.header().width;
    s.video_height = vbs2.header().height;
    s.time_base_num = vbt.header().time_base_num;
    s.time_base_den = vbt.header().time_base_den;

    // Derive current frame from renderer PTS
    if (g_get_current_pts_us) {
        int64_t pts_us = g_get_current_pts_us();
        s.current_frame_idx = mgr.current_frame_idx(pts_us);
    }

    return &s;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_frames(NakiFrameInfo* out, int32_t max_count) {
    auto& mgr = vr::analysis::AnalysisManager::instance();
    if (!mgr.is_loaded()) return 0;

    int count = std::min(max_count, std::min(mgr.vbs2().frame_count(), mgr.vbt().packet_count()));
    for (int i = 0; i < count; i++) {
        auto fh = mgr.vbs2().read_frame_header(i);
        const auto& pkt = mgr.vbt().entry(i);

        auto& f = out[i];
        f.poc = fh.poc;
        f.temporal_id = fh.temporal_id;
        f.slice_type = fh.slice_type;
        f.nal_type = fh.nal_unit_type;
        f.avg_qp = fh.avg_qp;
        f.num_ref_l0 = fh.num_ref_l0;
        f.num_ref_l1 = fh.num_ref_l1;
        std::memcpy(f.ref_pocs_l0, fh.ref_pocs_l0, sizeof(fh.ref_pocs_l0));
        std::memcpy(f.ref_pocs_l1, fh.ref_pocs_l1, sizeof(fh.ref_pocs_l1));
        f.pts = pkt.pts;
        f.dts = pkt.dts;
        f.packet_size = static_cast<int32_t>(pkt.size);
        f.keyframe = (pkt.flags & 0x01) ? 1 : 0;
    }
    return count;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_nalus(NakiNaluInfo* out, int32_t max_count) {
    auto& mgr = vr::analysis::AnalysisManager::instance();
    if (!mgr.is_loaded()) return 0;

    int count = std::min(max_count, mgr.vbi().nalu_count());
    for (int i = 0; i < count; i++) {
        const auto& e = mgr.vbi().entry(i);
        auto& n = out[i];
        n.offset = e.offset;
        n.size = e.size;
        n.nal_type = e.nal_type;
        n.temporal_id = e.temporal_id;
        n.layer_id = e.layer_id;
        n.flags = e.flags;
    }
    return count;
}

extern "C" __declspec(dllexport)
void naki_analysis_set_overlay(const NakiOverlayState* state) {
    auto& overlay = vr::analysis::AnalysisManager::instance().overlay;
    overlay.show_cu_grid = state->show_cu_grid != 0;
    overlay.show_pred_mode = state->show_pred_mode != 0;
    overlay.show_qp_heatmap = state->show_qp_heatmap != 0;
}
