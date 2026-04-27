#pragma once

#include "analysis/parsers/vbs2_parser.h"
#include "analysis/parsers/vbi_parser.h"
#include "analysis/parsers/vbt_parser.h"
#include <string>

namespace vr::analysis {

class AnalysisManager {
public:
    static AnalysisManager& instance();

    // Load specific analysis files (paths provided by Flutter)
    bool load(const std::string& vbs2_path,
              const std::string& vbi_path,
              const std::string& vbt_path);
    void unload();
    bool is_loaded() const { return loaded_; }

    const Vbs2File& vbs2() const { return vbs2_; }
    const VbiFile& vbi() const { return vbi_; }
    const VbtFile& vbt() const { return vbt_; }

    // Overlay state (written by Dart via FFI, read by render thread)
    struct OverlayState {
        bool show_cu_grid = false;
        bool show_pred_mode = false;
        bool show_qp_heatmap = false;
    };

    OverlayState overlay;
    const OverlayState& overlay_state() const { return overlay; }

    // Derive current frame index from PTS
    int current_frame_idx(int64_t pts_us) const;

private:
    AnalysisManager() = default;
    Vbs2File vbs2_;
    VbiFile vbi_;
    VbtFile vbt_;
    bool loaded_ = false;
};

} // namespace vr::analysis
