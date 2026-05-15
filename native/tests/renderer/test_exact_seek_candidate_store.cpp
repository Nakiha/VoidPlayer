#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/exact_seek_candidate_store.h"

#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

using namespace vr;

namespace {

ExactSeekCandidate make_candidate(int64_t pts) {
    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->pts = pts;
    return ExactSeekCandidate{
        pts,
        std::shared_ptr<AVFrame>(frame, [](AVFrame* f) {
            av_frame_free(&f);
        }),
    };
}

} // namespace

TEST_CASE("ExactSeekCandidateStore: collect keeps only latest pre-target frame",
          "[decode_thread][exact_seek_candidate_store]") {
    ExactSeekCandidateStore store;
    int snapshots = 0;
    auto snapshot = [&](ExactSeekCandidate&) { ++snapshots; };

    store.collect(make_candidate(10), 50, snapshot);
    store.collect(make_candidate(20), 50, snapshot);

    REQUIRE(store.reorder_count() == 1);
    REQUIRE(store.reorder_at(0).pts_us == 20);
    REQUIRE(snapshots == 0);
}

TEST_CASE("ExactSeekCandidateStore: first post-target frame snapshots previous preview",
          "[decode_thread][exact_seek_candidate_store]") {
    ExactSeekCandidateStore store;
    std::vector<int64_t> snapshotted_pts;
    auto snapshot = [&](ExactSeekCandidate& candidate) {
        snapshotted_pts.push_back(candidate.pts_us);
    };

    store.collect(make_candidate(40), 50, snapshot);
    store.collect(make_candidate(60), 50, snapshot);
    store.collect(make_candidate(80), 50, snapshot);
    store.collect(make_candidate(100), 50, snapshot);

    REQUIRE(store.reorder_count() == 4);
    REQUIRE(snapshotted_pts.size() == 1);
    REQUIRE(snapshotted_pts[0] == 40);
    REQUIRE(store.preview_window_ready(50));
}

TEST_CASE("ExactSeekCandidateStore: moves unpreviewed tail into pending queue",
          "[decode_thread][exact_seek_candidate_store]") {
    ExactSeekCandidateStore store;
    auto snapshot = [](ExactSeekCandidate&) {};
    store.collect(make_candidate(40), 50, snapshot);
    store.collect(make_candidate(60), 50, snapshot);
    store.collect(make_candidate(80), 50, snapshot);

    store.move_reorder_tail_to_pending(1);

    REQUIRE(store.reorder_empty());
    REQUIRE(store.pending_count() == 2);
    auto first = store.pop_pending();
    REQUIRE(first.has_value());
    REQUIRE(first->pts_us == 60);
    REQUIRE(store.pending_count() == 1);
}

TEST_CASE("ExactSeekCandidateStore: memory stats track stable frame count",
          "[decode_thread][exact_seek_candidate_store]") {
    ExactSeekCandidateStore store;
    auto snapshot = [](ExactSeekCandidate& candidate) {
        TextureFrame stable;
        stable.texture_handle = reinterpret_cast<void*>(0x1);
        candidate.stable_frame = stable;
    };

    store.collect(make_candidate(60), 50, snapshot);

    auto stats = store.stats_snapshot();
    REQUIRE(stats.reorder_count == 1);
    REQUIRE(stats.pending_count == 0);
    REQUIRE(stats.stable_frame_count == 1);
}
