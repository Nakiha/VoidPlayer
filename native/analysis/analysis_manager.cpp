#include "analysis/analysis_manager.h"

#include <utility>

namespace vr::analysis {

AnalysisManager& AnalysisManager::instance() {
    static AnalysisManager mgr;
    return mgr;
}

bool AnalysisManager::load(const std::string& analysis_path) {
    unload();
    return load_vac2(analysis_path);
}

bool AnalysisManager::load_vac2(const std::string& analysis_path) {
    auto session = std::make_shared<AnalysisSession>();
    if (!session->open(analysis_path)) return false;
    std::lock_guard<std::mutex> lock(session_mutex_);
    session_ = std::move(session);
    return true;
}

void AnalysisManager::unload() {
    clear_overlay_tracks();
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_.reset();
    }
    overlay.show_cu_grid.store(false, std::memory_order_release);
    overlay.show_pred_mode.store(false, std::memory_order_release);
    overlay.show_qp_heatmap.store(false, std::memory_order_release);
    overlay.show_pred_lines.store(false, std::memory_order_release);
    overlay.show_cu_bit_cost_heatmap.store(false, std::memory_order_release);
    overlay.opacity_permille.store(550, std::memory_order_release);
    overlay.mode.store(0, std::memory_order_release);
    overlay.track_file_id.store(-1, std::memory_order_release);
}

bool AnalysisManager::is_loaded() const {
    return session_snapshot() != nullptr;
}

std::shared_ptr<const AnalysisSession> AnalysisManager::session_snapshot() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return session_;
}

int AnalysisManager::frame_count() const {
    const auto session = session_snapshot();
    if (!session) return 0;
    return session->frame_count();
}

uint32_t AnalysisManager::video_width() const {
    const auto session = session_snapshot();
    if (!session) return 0;
    return session->video_width();
}

uint32_t AnalysisManager::video_height() const {
    const auto session = session_snapshot();
    if (!session) return 0;
    return session->video_height();
}

VachunkFrameSummary AnalysisManager::read_frame_summary(int frame_idx) const {
    const auto session = session_snapshot();
    return session ? session->read_frame_summary(frame_idx) : VachunkFrameSummary{};
}

VachunkOverlayFrameData AnalysisManager::read_overlay_frame(int frame_idx) const {
    const auto session = session_snapshot();
    return session ? session->read_overlay_frame(frame_idx) : VachunkOverlayFrameData{};
}

bool AnalysisManager::set_overlay_track(int track_file_id, const std::string& analysis_path) {
    return overlay_tracks_.set_track(track_file_id, analysis_path);
}

void AnalysisManager::clear_overlay_tracks() {
    overlay_tracks_.clear();
}

std::vector<std::pair<int, std::shared_ptr<const AnalysisSession>>>
AnalysisManager::overlay_track_snapshot() const {
    return overlay_tracks_.snapshot();
}

int AnalysisManager::current_frame_idx(int64_t pts_us) const {
    const auto session = session_snapshot();
    if (!session) return -1;
    return session->current_frame_idx(pts_us);
}

} // namespace vr::analysis
