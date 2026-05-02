#pragma once

#include "analysis/parsers/analysis_container.h"
#include "analysis/parsers/vbs3_parser.h"
#include "analysis/parsers/vbi_parser.h"
#include "analysis/parsers/vbt_parser.h"
#include <atomic>
#include <string>

namespace vr::analysis {

class AnalysisManager {
public:
    AnalysisManager() = default;
    static AnalysisManager& instance();

    bool load(const std::string& analysis_path);
    void unload();
    bool is_loaded() const { return loaded_; }

    const Vbs3File& vbs3() const { return vbs3_; }
    const VbiFile& vbi() const { return vbi_; }
    const VbtFile& vbt() const { return vbt_; }

    // Overlay state (written by Dart via FFI, read by render thread)
    struct OverlayState {
        std::atomic<bool> show_cu_grid{false};
        std::atomic<bool> show_pred_mode{false};
        std::atomic<bool> show_qp_heatmap{false};
    };

    OverlayState overlay;
    const OverlayState& overlay_state() const { return overlay; }

    // Derive current frame index from PTS
    int current_frame_idx(int64_t pts_us) const;

private:
    AnalysisContainerFile container_;
    Vbs3File vbs3_;
    VbiFile vbi_;
    VbtFile vbt_;
    bool loaded_ = false;
};

} // namespace vr::analysis
