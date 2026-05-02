#include "analysis/analysis_manager.h"

#include <limits>

namespace vr::analysis {

AnalysisManager& AnalysisManager::instance() {
    static AnalysisManager mgr;
    return mgr;
}

bool AnalysisManager::load(const std::string& vbs3_path,
                            const std::string& vbi_path,
                            const std::string& vbt_path) {
    unload();

    // VBS3 is optional because VTM block statistics are currently VVC-only.
    if (!vbs3_path.empty()) {
        vbs3_.open(vbs3_path);  // failure is OK; VBI/VBT fallback remains available
    }

    if (!vbi_.open(vbi_path)) { vbs3_.close(); return false; }
    if (!vbt_.open(vbt_path)) { vbs3_.close(); vbi_.close(); return false; }

    loaded_ = true;
    return true;
}

void AnalysisManager::unload() {
    vbs3_.close();
    vbi_.close();
    vbt_.close();
    loaded_ = false;
    overlay.show_cu_grid.store(false, std::memory_order_release);
    overlay.show_pred_mode.store(false, std::memory_order_release);
    overlay.show_qp_heatmap.store(false, std::memory_order_release);
}

int AnalysisManager::current_frame_idx(int64_t pts_us) const {
    if (!loaded_) return -1;
    // Convert microseconds to time_base units
    const auto& h = vbt_.header();
    if (h.time_base_num == 0 || h.time_base_den == 0) return -1;
    const long double pts_tb =
        static_cast<long double>(pts_us) *
        static_cast<long double>(h.time_base_den) /
        (static_cast<long double>(h.time_base_num) * 1000000.0L);
    if (pts_tb < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        pts_tb > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return -1;
    }
    return vbt_.packet_at_pts(static_cast<int64_t>(pts_tb));
}

} // namespace vr::analysis
