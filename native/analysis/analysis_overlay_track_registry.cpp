#include "analysis/analysis_overlay_track_registry.h"

#include <utility>

namespace vr::analysis {

bool AnalysisOverlayTrackRegistry::set_track(int track_file_id,
                                             const std::string& analysis_path) {
    if (track_file_id < 0) return false;
    auto session = std::make_shared<AnalysisSession>();
    if (!session->open(analysis_path)) {
        return false;
    }
    session->load_overlay_chunk_index();
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_[track_file_id] = std::move(session);
    return true;
}

void AnalysisOverlayTrackRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_.clear();
}

std::vector<std::pair<int, std::shared_ptr<const AnalysisSession>>>
AnalysisOverlayTrackRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<int, std::shared_ptr<const AnalysisSession>>> tracks;
    tracks.reserve(tracks_.size());
    for (const auto& [track_file_id, session] : tracks_) {
        tracks.emplace_back(track_file_id, session);
    }
    return tracks;
}

} // namespace vr::analysis
