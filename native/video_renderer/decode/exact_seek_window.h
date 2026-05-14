#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

namespace vr {

constexpr int64_t kExactSeekLookbehindUs = 250000;
constexpr size_t kExactSeekPreviewWindowFrames = 4;

bool is_exact_seek_pre_target(int64_t pts_us, int64_t target_pts_us);
bool should_collect_exact_seek_candidate(int64_t pts_us, int64_t target_pts_us);
bool is_exact_seek_preview_window_ready(int64_t target_pts_us,
                                        size_t candidate_count,
                                        int64_t newest_pts_us,
                                        size_t min_window = kExactSeekPreviewWindowFrames);
std::optional<size_t> select_exact_seek_preview_index(
    const std::vector<int64_t>& candidate_pts_us,
    int64_t target_pts_us);

} // namespace vr
