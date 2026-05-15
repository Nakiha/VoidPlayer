#include "video_renderer/decode/exact_seek_candidate_store.h"

#include "video_renderer/decode/exact_seek_window.h"

#include <spdlog/spdlog.h>

#include <algorithm>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace vr {
namespace {

uint64_t estimate_av_frame_cpu_bytes(const AVFrame* frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0 ||
        frame->format == AV_PIX_FMT_NONE || frame->hw_frames_ctx) {
        return 0;
    }
    const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    if (!desc) {
        return 0;
    }

    int max_plane = 0;
    for (int i = 0; i < desc->nb_components; ++i) {
        max_plane = std::max(max_plane, static_cast<int>(desc->comp[i].plane));
    }

    uint64_t bytes = 0;
    for (int plane = 0; plane <= max_plane && plane < AV_NUM_DATA_POINTERS; ++plane) {
        if (!frame->data[plane] || frame->linesize[plane] == 0) {
            continue;
        }
        const bool chroma_plane = plane == 1 || plane == 2;
        const int shift = chroma_plane ? desc->log2_chroma_h : 0;
        const int plane_height = (frame->height + (1 << shift) - 1) >> shift;
        const int stride = frame->linesize[plane] < 0
            ? -frame->linesize[plane]
            : frame->linesize[plane];
        bytes += static_cast<uint64_t>(stride) * static_cast<uint64_t>(plane_height);
    }
    return bytes;
}

} // namespace

ExactSeekCandidate ExactSeekCandidateStore::make_candidate(AVFrame* frame) {
    AVFrame* cloned = av_frame_clone(frame);
    if (!cloned) {
        spdlog::error("[DecodeThread] Failed to clone exact-seek candidate frame");
        return {};
    }
    return ExactSeekCandidate{
        frame->pts,
        std::shared_ptr<AVFrame>(cloned, [](AVFrame* f) {
            av_frame_free(&f);
        }),
    };
}

void ExactSeekCandidateStore::clear() {
    reorder_.clear();
    pending_.clear();
    refresh_stats();
}

void ExactSeekCandidateStore::clear_reorder() {
    reorder_.clear();
    refresh_stats();
}

void ExactSeekCandidateStore::clear_pending() {
    pending_.clear();
    refresh_stats();
}

bool ExactSeekCandidateStore::reorder_empty() const {
    return reorder_.empty();
}

bool ExactSeekCandidateStore::pending_empty() const {
    return pending_.empty();
}

size_t ExactSeekCandidateStore::reorder_count() const {
    return reorder_.size();
}

size_t ExactSeekCandidateStore::pending_count() const {
    return pending_.size();
}

const ExactSeekCandidate& ExactSeekCandidateStore::reorder_at(size_t index) const {
    return reorder_.at(index);
}

ExactSeekCandidate& ExactSeekCandidateStore::reorder_at(size_t index) {
    return reorder_.at(index);
}

const std::vector<ExactSeekCandidate>& ExactSeekCandidateStore::reorder_candidates() const {
    return reorder_;
}

std::vector<ExactSeekCandidate>& ExactSeekCandidateStore::reorder_candidates() {
    return reorder_;
}

void ExactSeekCandidateStore::collect(ExactSeekCandidate candidate,
                                      int64_t target_us,
                                      const SnapshotCallback& snapshot_if_needed) {
    if (!candidate.frame) {
        return;
    }
    if (target_us >= 0 && candidate.pts_us < target_us) {
        reorder_.clear();
    } else if (target_us >= 0 && reorder_.empty()) {
        if (snapshot_if_needed) {
            snapshot_if_needed(candidate);
        }
    } else if (target_us >= 0 &&
               !reorder_.empty() &&
               reorder_.back().pts_us < target_us &&
               !reorder_.back().stable_frame.has_value()) {
        if (snapshot_if_needed) {
            snapshot_if_needed(reorder_.back());
        }
    }
    reorder_.push_back(std::move(candidate));
    refresh_stats();
}

bool ExactSeekCandidateStore::preview_window_ready(int64_t target_us) const {
    const int64_t newest_pts_us = reorder_.empty() ? 0 : reorder_.back().pts_us;
    return is_exact_seek_preview_window_ready(target_us, reorder_.size(), newest_pts_us);
}

std::vector<int64_t> ExactSeekCandidateStore::reorder_pts() const {
    std::vector<int64_t> pts;
    pts.reserve(reorder_.size());
    for (const auto& candidate : reorder_) {
        pts.push_back(candidate.pts_us);
    }
    return pts;
}

std::optional<ExactSeekCandidate> ExactSeekCandidateStore::pop_pending() {
    if (pending_.empty()) {
        return std::nullopt;
    }
    auto candidate = std::move(pending_.front());
    pending_.pop_front();
    refresh_stats();
    return candidate;
}

void ExactSeekCandidateStore::move_reorder_tail_to_pending(size_t begin) {
    pending_.clear();
    for (size_t i = begin; i < reorder_.size(); ++i) {
        pending_.push_back(std::move(reorder_[i]));
    }
    reorder_.clear();
    refresh_stats();
}

ExactSeekCandidateMemoryStats ExactSeekCandidateStore::stats_snapshot() const {
    return ExactSeekCandidateMemoryStats{
        reorder_count_.load(std::memory_order_relaxed),
        pending_count_.load(std::memory_order_relaxed),
        stable_frame_count_.load(std::memory_order_relaxed),
        candidate_cpu_bytes_.load(std::memory_order_relaxed),
        stable_cpu_bytes_.load(std::memory_order_relaxed),
    };
}

void ExactSeekCandidateStore::refresh_stats() {
    size_t stable_count = 0;
    uint64_t candidate_cpu_bytes = 0;
    uint64_t stable_cpu_bytes = 0;
    auto visit = [&](const ExactSeekCandidate& candidate) {
        candidate_cpu_bytes += estimate_av_frame_cpu_bytes(candidate.frame.get());
        if (candidate.stable_frame.has_value()) {
            ++stable_count;
            stable_cpu_bytes += estimate_texture_frame_cpu_bytes(*candidate.stable_frame);
        }
    };
    for (const auto& candidate : reorder_) {
        visit(candidate);
    }
    for (const auto& candidate : pending_) {
        visit(candidate);
    }
    reorder_count_.store(reorder_.size(), std::memory_order_relaxed);
    pending_count_.store(pending_.size(), std::memory_order_relaxed);
    stable_frame_count_.store(stable_count, std::memory_order_relaxed);
    candidate_cpu_bytes_.store(candidate_cpu_bytes, std::memory_order_relaxed);
    stable_cpu_bytes_.store(stable_cpu_bytes, std::memory_order_relaxed);
}

} // namespace vr
