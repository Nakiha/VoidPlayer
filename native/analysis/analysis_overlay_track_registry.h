#pragma once

#include "analysis/analysis_session.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vr::analysis {

class AnalysisOverlayTrackRegistry {
public:
    bool set_track(int track_file_id, const std::string& analysis_path);
    void clear();

    std::vector<std::pair<int, std::shared_ptr<const AnalysisSession>>>
    snapshot() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<int, std::shared_ptr<const AnalysisSession>> tracks_;
};

} // namespace vr::analysis
