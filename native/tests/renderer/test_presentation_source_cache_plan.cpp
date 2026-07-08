#include <catch2/catch_test_macros.hpp>

#include "renderer/render/presentation_source_cache_plan.h"

namespace {

vr::TrackInfo make_track(int slot,
                         int file_id,
                         int width,
                         int height,
                         int transfer) {
    vr::TrackInfo info;
    info.slot = slot;
    info.file_id = file_id;
    info.width = width;
    info.height = height;
    info.color.transfer = transfer;
    return info;
}

} // namespace

TEST_CASE("Presentation source cache plan builds stable ordered descriptors",
          "[presentation_source_cache_plan]") {
    vr::PresentationSourceCacheRequest request;
    request.requested_slots = {true, true, false, true};
    request.source_order = {3, 0, 1, 2};
    const std::vector<vr::TrackInfo> tracks = {
        make_track(3, 30, 3840, 2160, 16),
        make_track(0, 10, 1920, 1080, 1),
        make_track(1, 20, 1280, 720, 13),
    };

    const auto plan =
        vr::build_presentation_source_cache_plan(request, tracks);

    REQUIRE(plan.complete);
    REQUIRE(plan.error == "none");
    REQUIRE(plan.tracks.size() == 3);
    REQUIRE(plan.tracks[0].slot == 0);
    REQUIRE(plan.tracks[1].slot == 1);
    REQUIRE(plan.tracks[2].slot == 3);
    REQUIRE(plan.signature ==
            "R16G16B16A16_FLOAT|3,0,1,2,|0:10:1920x1080:1|"
            "1:20:1280x720:13|3:30:3840x2160:16");
}

TEST_CASE("Presentation source cache plan reports incomplete topology",
          "[presentation_source_cache_plan]") {
    vr::PresentationSourceCacheRequest request;
    request.requested_slots = {true, false, true, false};
    const std::vector<vr::TrackInfo> tracks = {
        make_track(0, 10, 1920, 1080, 1),
    };

    const auto plan =
        vr::build_presentation_source_cache_plan(request, tracks);

    REQUIRE_FALSE(plan.complete);
    REQUIRE(plan.error == "source-cache-track-mismatch");
    REQUIRE(plan.signature == "slots=0,2,|tracks=1");
}

TEST_CASE("Presentation source cache plan rejects invalid live track geometry",
          "[presentation_source_cache_plan]") {
    vr::PresentationSourceCacheRequest request;
    request.requested_slots = {true, false, false, false};
    const std::vector<vr::TrackInfo> tracks = {
        make_track(0, 10, 0, 1080, 1),
    };

    const auto plan =
        vr::build_presentation_source_cache_plan(request, tracks);

    REQUIRE_FALSE(plan.complete);
    REQUIRE(plan.error == "source-cache-invalid-track");
}
