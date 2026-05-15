#pragma once

#include "video_renderer/buffer/bidi_ring_buffer.h"
#include "video_renderer/renderer_limits.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

struct AVFrame;

namespace vr {

struct ExactSeekCandidate {
    int64_t pts_us = 0;
    std::shared_ptr<AVFrame> frame;
    std::optional<TextureFrame> stable_frame;
};

struct ExactSeekCandidateMemoryStats {
    size_t reorder_count = 0;
    size_t pending_count = 0;
    size_t stable_frame_count = 0;
    size_t dropped_by_budget_count = 0;
    uint64_t candidate_cpu_bytes = 0;
    uint64_t stable_cpu_bytes = 0;
};

class ExactSeekCandidateStore {
public:
    using SnapshotCallback = std::function<void(ExactSeekCandidate&)>;

    explicit ExactSeekCandidateStore(
        size_t max_reorder_frames =
            default_native_resource_budget().max_exact_seek_reorder_frames);

    static ExactSeekCandidate make_candidate(AVFrame* frame);

    void clear();
    void clear_reorder();
    void clear_pending();

    bool reorder_empty() const;
    bool pending_empty() const;
    size_t reorder_count() const;
    size_t pending_count() const;

    const ExactSeekCandidate& reorder_at(size_t index) const;
    ExactSeekCandidate& reorder_at(size_t index);
    const std::vector<ExactSeekCandidate>& reorder_candidates() const;
    std::vector<ExactSeekCandidate>& reorder_candidates();

    void collect(ExactSeekCandidate candidate,
                 int64_t target_us,
                 const SnapshotCallback& snapshot_if_needed);
    bool preview_window_ready(int64_t target_us) const;
    std::vector<int64_t> reorder_pts() const;
    std::optional<ExactSeekCandidate> pop_pending();
    void move_reorder_tail_to_pending(size_t begin);

    ExactSeekCandidateMemoryStats stats_snapshot() const;

private:
    void refresh_stats();

    size_t max_reorder_frames_ = kMaxExactSeekReorderFrames;
    std::vector<ExactSeekCandidate> reorder_;
    std::deque<ExactSeekCandidate> pending_;
    std::atomic<size_t> reorder_count_{0};
    std::atomic<size_t> pending_count_{0};
    std::atomic<size_t> stable_frame_count_{0};
    std::atomic<size_t> dropped_by_budget_count_{0};
    std::atomic<uint64_t> candidate_cpu_bytes_{0};
    std::atomic<uint64_t> stable_cpu_bytes_{0};
};

} // namespace vr
