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

    if (const auto* vbs4 = container_.section("VBS4")) {
        vbs4_.open_region(path, vbs4->offset, vbs4->size);
    }

    loaded_ = true;
    return true;
}

void AnalysisManager::unload() {
    clear_overlay_tracks();
    vbs4_.close();
    vbi_.close();
    vbt_.close();
    container_.close();
    loaded_ = false;
    overlay.show_cu_grid.store(false, std::memory_order_release);
    overlay.show_pred_mode.store(false, std::memory_order_release);
    overlay.show_qp_heatmap.store(false, std::memory_order_release);
    overlay.show_pred_lines.store(false, std::memory_order_release);
    overlay.show_cu_bit_cost_heatmap.store(false, std::memory_order_release);
    overlay.opacity_permille.store(550, std::memory_order_release);
    overlay.mode.store(0, std::memory_order_release);
    overlay.track_file_id.store(-1, std::memory_order_release);
}

bool AnalysisManager::set_overlay_track(int track_file_id, const std::string& analysis_path) {
    if (track_file_id < 0) return false;
    auto track_manager = std::make_shared<AnalysisManager>();
    if (!track_manager->load(analysis_path)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(overlay_tracks_mutex_);
    overlay_tracks_[track_file_id] = std::move(track_manager);
    return true;
}

void AnalysisManager::clear_overlay_tracks() {
    std::lock_guard<std::mutex> lock(overlay_tracks_mutex_);
    overlay_tracks_.clear();
}

std::vector<std::pair<int, std::shared_ptr<const AnalysisManager>>>
AnalysisManager::overlay_track_snapshot() const {
    std::lock_guard<std::mutex> lock(overlay_tracks_mutex_);
    std::vector<std::pair<int, std::shared_ptr<const AnalysisManager>>> tracks;
    tracks.reserve(overlay_tracks_.size());
    for (const auto& [track_file_id, manager] : overlay_tracks_) {
        tracks.emplace_back(track_file_id, manager);
    }
    return tracks;
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
