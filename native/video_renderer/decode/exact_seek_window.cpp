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
    int64_t target_pts_us,
    bool prefer_after_target) {
    if (target_pts_us < 0 || candidate_pts_us.empty()) {
        return std::nullopt;
    }

    size_t selected = 0;
    bool found_preferred = false;
    for (size_t i = 0; i < candidate_pts_us.size(); ++i) {
        const auto pts = candidate_pts_us[i];
        if (prefer_after_target) {
            if (pts >= target_pts_us &&
                (!found_preferred || pts < candidate_pts_us[selected])) {
                selected = i;
                found_preferred = true;
            }
            continue;
        }

        if (pts <= target_pts_us &&
            (!found_preferred || pts > candidate_pts_us[selected])) {
            selected = i;
            found_preferred = true;
        }
    }
    if (prefer_after_target && !found_preferred) {
        for (size_t i = 0; i < candidate_pts_us.size(); ++i) {
            const auto pts = candidate_pts_us[i];
            if (pts <= target_pts_us &&
                (i == 0 || pts > candidate_pts_us[selected])) {
                selected = i;
            }
        }
    }
    return selected;
}

} // namespace vr
