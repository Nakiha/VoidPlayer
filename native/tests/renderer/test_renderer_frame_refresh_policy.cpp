#include <catch2/catch_test_macros.hpp>

#include "renderer/render/renderer_frame_refresh_policy.h"

using namespace vr;

TEST_CASE("RendererFrameRefreshPolicy: analysis overlay retains the presented frame",
          "[renderer][frame_refresh][analysis_overlay]") {
    const auto policy =
        renderer_frame_refresh_policy("windows-analysis-overlay-state");

    REQUIRE(policy.retain_presented_frame);
    REQUIRE_FALSE(policy.decoded_preview_refresh);
    REQUIRE_FALSE(policy.interaction_refresh);
}

TEST_CASE("RendererFrameRefreshPolicy: only seek preview consumes decoded preview",
          "[renderer][frame_refresh][seek]") {
    const auto seek = renderer_frame_refresh_policy("seek_frame_refresh");
    REQUIRE_FALSE(seek.retain_presented_frame);
    REQUIRE(seek.decoded_preview_refresh);

    const auto generic = renderer_frame_refresh_policy("windows-runner-resize");
    REQUIRE_FALSE(generic.retain_presented_frame);
    REQUIRE_FALSE(generic.decoded_preview_refresh);
}

TEST_CASE("RendererFrameRefreshPolicy: incomplete interaction snapshots retry without failure",
          "[renderer][frame_refresh][interaction]") {
    REQUIRE(classify_interaction_refresh_result(
                RendererFrameRefreshResult::Presented, false) ==
            RendererInteractionRefreshDisposition::Presented);
    REQUIRE(classify_interaction_refresh_result(
                RendererFrameRefreshResult::NotReady, false) ==
            RendererInteractionRefreshDisposition::RetryNotReady);
    REQUIRE(classify_interaction_refresh_result(
                RendererFrameRefreshResult::Failed, true) ==
            RendererInteractionRefreshDisposition::RetryBackpressure);
    REQUIRE(classify_interaction_refresh_result(
                RendererFrameRefreshResult::Failed, false) ==
            RendererInteractionRefreshDisposition::Failed);
}
