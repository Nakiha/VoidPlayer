#include "renderer/render/renderer_present_history.h"

#include <catch2/catch_test_macros.hpp>

namespace {

vr::PresentDecision make_decision(int file_id, int64_t pts_us) {
    vr::PresentDecision decision;
    decision.should_present = true;
    decision.current_pts_us = pts_us;
    decision.file_ids[0] = file_id;
    decision.track_generations[0] = 7;
    vr::TextureFrame frame;
    frame.pts_us = pts_us;
    frame.dts_us = pts_us - 1000;
    frame.analysis_frame_index = 3;
    frame.source_packet_index = 4;
    frame.source_packet_pos = 5;
    frame.frame_identity_mode = vr::FrameIdentityMode::TimestampEstimated;
    decision.frames[0] = frame;
    return decision;
}

} // namespace

TEST_CASE("Present history records source cache publish anchors",
          "[windows_source_cache][windows_source_projection]") {
    vr::RendererPresentHistory history;
    auto decision = make_decision(11, 1200000);

    history.set_source_cache_published(decision, 4, 9);

    const auto snapshot = history.snapshot();
    REQUIRE(snapshot.frames[0].has_value());
    REQUIRE(snapshot.frames[0]->pts_us == 1200000);
    const auto diagnostics = history.diagnostics();
    REQUIRE(diagnostics.mode ==
            vr::RendererPresentedAnchorMode::SourceCachePublish);
    REQUIRE(diagnostics.source_cache_ring_generation == 4);
    REQUIRE(diagnostics.source_cache_frame_generation == 9);
    REQUIRE(diagnostics.source_cache_publish_count == 1);
}

TEST_CASE("Present history keeps source cache mode for the same viewport frame",
          "[windows_source_cache][windows_source_projection]") {
    vr::RendererPresentHistory history;
    auto decision = make_decision(11, 1200000);

    history.set_source_cache_published(decision, 4, 9);
    history.set(decision);

    const auto diagnostics = history.diagnostics();
    REQUIRE(diagnostics.mode ==
            vr::RendererPresentedAnchorMode::SourceCachePublish);
    REQUIRE(diagnostics.source_cache_ring_generation == 4);
    REQUIRE(diagnostics.source_cache_frame_generation == 9);
    REQUIRE(diagnostics.source_cache_publish_count == 1);
}

TEST_CASE("Present history treats a different viewport frame as viewport present",
          "[windows_source_cache][windows_source_projection]") {
    vr::RendererPresentHistory history;
    history.set_source_cache_published(make_decision(11, 1200000), 4, 9);

    history.set(make_decision(11, 1500000));

    const auto snapshot = history.snapshot();
    REQUIRE(snapshot.frames[0].has_value());
    REQUIRE(snapshot.frames[0]->pts_us == 1500000);
    const auto diagnostics = history.diagnostics();
    REQUIRE(diagnostics.mode == vr::RendererPresentedAnchorMode::ViewportPresent);
    REQUIRE(diagnostics.source_cache_ring_generation == 0);
    REQUIRE(diagnostics.source_cache_frame_generation == 0);
    REQUIRE(diagnostics.source_cache_publish_count == 1);
}
