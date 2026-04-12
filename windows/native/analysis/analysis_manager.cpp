#include "analysis/analysis_manager.h"

namespace vr::analysis {

AnalysisManager& AnalysisManager::instance() {
    static AnalysisManager mgr;
    return mgr;
}

bool AnalysisManager::load(const std::string& vbs2_path,
                            const std::string& vbi_path,
                            const std::string& vbt_path) {
    unload();

    if (!vbs2_.open(vbs2_path)) return false;
    if (!vbi_.open(vbi_path)) { vbs2_.close(); return false; }
    if (!vbt_.open(vbt_path)) { vbs2_.close(); vbi_.close(); return false; }

    loaded_ = true;
    return true;
}

void AnalysisManager::unload() {
    vbs2_.close();
    vbi_.close();
    vbt_.close();
    loaded_ = false;
    overlay = {};
}

int AnalysisManager::current_frame_idx(int64_t pts_us) const {
    if (!loaded_) return -1;
    // Convert microseconds to time_base units
    const auto& h = vbt_.header();
    if (h.time_base_den == 0) return -1;
    int64_t pts_tb = pts_us * h.time_base_num / (1000000LL / h.time_base_den);
    return vbt_.packet_at_pts(pts_tb);
}

} // namespace vr::analysis
