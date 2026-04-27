#include "analysis/analysis_manager.h"

#include <limits>

namespace vr::analysis {

AnalysisManager& AnalysisManager::instance() {
    static AnalysisManager mgr;
    return mgr;
}

bool AnalysisManager::load(const std::string& vbs2_path,
                            const std::string& vbi_path,
                            const std::string& vbt_path) {
    unload();

    // VBS2 is optional (requires VTM decoder instrumentation)
    if (!vbs2_path.empty()) {
        vbs2_.open(vbs2_path);  // failure is OK
    }

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
