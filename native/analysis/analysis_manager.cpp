#include "analysis/analysis_manager.h"

#include <limits>

namespace vr::analysis {

AnalysisManager& AnalysisManager::instance() {
    static AnalysisManager mgr;
    return mgr;
}

bool AnalysisManager::load(const std::string& analysis_path) {
    unload();

    if (!container_.open(analysis_path)) return false;

    const auto* vbi = container_.section("VBI2");
    const auto* vbt = container_.section("VBT1");
    if (!vbi || !vbt) { unload(); return false; }

    const auto& path = container_.path();
    if (!vbi_.open_region(path, vbi->offset, vbi->size)) { unload(); return false; }
    if (!vbt_.open_region(path, vbt->offset, vbt->size)) { unload(); return false; }

    if (const auto* vbs3 = container_.section("VBS3")) {
        vbs3_.open_region(path, vbs3->offset, vbs3->size);
    }

    loaded_ = true;
    return true;
}

void AnalysisManager::unload() {
    vbs3_.close();
    vbi_.close();
    vbt_.close();
    container_.close();
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
