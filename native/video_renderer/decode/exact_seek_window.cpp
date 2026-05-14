#include "video_renderer/decode/exact_seek_window.h"

namespace vr {

bool is_exact_seek_pre_target(int64_t pts_us, int64_t target_pts_us) {
    return target_pts_us >= 0 && pts_us < target_pts_us;
}

bool should_collect_exact_seek_candidate(int64_t pts_us, int64_t target_pts_us) {
    return target_pts_us >= 0 && pts_us >= target_pts_us - kExactSeekLookbehindUs;
}

bool is_exact_seek_preview_window_ready(int64_t target_pts_us,
                                        size_t candidate_count,
                                        int64_t newest_pts_us,
                                        size_t min_window) {
    if (target_pts_us < 0 || candidate_count == 0) {
        return false;
    }
    if (newest_pts_us < target_pts_us) {
        return false;
    }
    return candidate_count >= min_window;
}

std::optional<size_t> select_exact_seek_preview_index(
    const std::vector<int64_t>& candidate_pts_us,
    int64_t target_pts_us) {
    if (target_pts_us < 0 || candidate_pts_us.empty()) {
        return std::nullopt;
    }

    size_t selected = 0;
    bool found_before_target = false;
    for (size_t i = 0; i < candidate_pts_us.size(); ++i) {
        if (candidate_pts_us[i] < target_pts_us) {
            selected = i;
            found_before_target = true;
        } else {
            break;
        }
    }
    if (!found_before_target) {
        selected = 0;
    }
    return selected;
}

} // namespace vr
