#pragma once

#include "analysis/parsers/analysis_container.h"
#include "analysis/parsers/vbs4_parser.h"
#include "analysis/parsers/vbi_parser.h"
#include "analysis/parsers/vbt_parser.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vr::analysis {

class AnalysisManager {
public:
    AnalysisManager() = default;
    static AnalysisManager& instance();

    bool load(const std::string& analysis_path);
    void unload();
    bool is_loaded() const { return loaded_; }

    const Vbs4File& vbs4() const { return vbs4_; }
    const VbiFile& vbi() const { return vbi_; }
    const VbtFile& vbt() const { return vbt_; }
    int frame_count() const { return vbs4_.frame_count(); }
    uint32_t video_width() const { return vbs4_.header().width; }
    uint32_t video_height() const { return vbs4_.header().height; }
    Vbs4FrameSummary read_frame_summary(int frame_idx) const {
        return vbs4_.read_frame_summary(frame_idx);
    }

    // Overlay state (written by Dart via FFI, read by render thread)
    struct OverlayState {
        std::atomic<bool> show_cu_grid{false};
        std::atomic<bool> show_pred_mode{false};
        std::atomic<bool> show_qp_heatmap{false};
        std::atomic<bool> show_pred_lines{false};
        std::atomic<bool> show_cu_bit_cost_heatmap{false};
        std::atomic<int> opacity_permille{550};
        std::atomic<int> mode{0};
        std::atomic<int> track_file_id{-1};
    };

    OverlayState overlay;
    const OverlayState& overlay_state() const { return overlay; }

    bool set_overlay_track(int track_file_id, const std::string& analysis_path);
    void clear_overlay_tracks();
    std::vector<std::pair<int, std::shared_ptr<const AnalysisManager>>>
    overlay_track_snapshot() const;

    // Derive current frame index from PTS
    int current_frame_idx(int64_t pts_us) const;

private:
    AnalysisContainerFile container_;
    Vbs4File vbs4_;
    VbiFile vbi_;
    VbtFile vbt_;
    bool loaded_ = false;
    mutable std::mutex overlay_tracks_mutex_;
    std::unordered_map<int, std::shared_ptr<AnalysisManager>> overlay_tracks_;
};

} // namespace vr::analysis
