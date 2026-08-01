#include "analysis/quality/quality_metrics.h"
#include "analysis/quality/quality_event_aggregator.h"
#include "analysis/quality/quality_video_analyzer.h"
#include "analysis/quality/quality_wgpu_backend.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef VIDEO_TEST_DIR
#define VIDEO_TEST_DIR ""
#endif

namespace {

using vr::analysis::quality::LumaPlaneView;
using vr::analysis::quality::LumaTemporalSignature;

LumaPlaneView make_u8_view(const std::vector<uint8_t>& pixels,
                           int width,
                           int height) {
    LumaPlaneView view;
    view.data = pixels.data();
    view.width = width;
    view.height = height;
    view.stride_bytes = width;
    view.sample_step_bytes = 1;
    view.bit_depth = 8;
    return view;
}

}  // namespace

TEST_CASE("quality video analyzer cancellation reaches media input",
          "[analysis][quality][cancel]") {
    using namespace vr::analysis::quality;
    bool cancel = false;
    std::vector<QualityAnalysisPhase> phases;
    QualityVideoAnalyzerOptions options;
    options.backend = QualityComputeBackend::Cpu;
    options.max_samples = 2;
    options.cancel_requested = [&cancel]() { return cancel; };
    options.progress_callback =
        [&](const QualityAnalysisProgress& progress) {
            phases.push_back(progress.phase);
            if (progress.phase == QualityAnalysisPhase::Decoding) {
                cancel = true;
            }
        };

    QualityReport report;
    std::string error;
    REQUIRE_FALSE(analyze_video_quality(
        std::string(VIDEO_TEST_DIR) + "/h265_10s_1920x1080.mp4",
        options,
        report,
        error));
    REQUIRE(error == kQualityAnalysisCancelledError);
    REQUIRE(phases.size() >= 2);
    REQUIRE(phases.front() == QualityAnalysisPhase::Opening);
    REQUIRE(std::find(
                phases.begin(),
                phases.end(),
                QualityAnalysisPhase::Decoding) != phases.end());
}

TEST_CASE("quality timeline normalization reindexes samples after PTS sort",
          "[analysis][quality][timeline]") {
    using namespace vr::analysis::quality;
    QualityReport report;
    for (const auto [sample_index, decoded_frame_index, pts_us] :
         std::array<std::array<int64_t, 3>, 4>{{
             {{0, 10, 2'000}},
             {{1, 11, 1'000}},
             {{2, 12, 1'000}},
             {{3, 13, 3'000}},
         }}) {
        FrameQualitySample sample;
        sample.sample_index = static_cast<uint64_t>(sample_index);
        sample.decoded_frame_index =
            static_cast<uint64_t>(decoded_frame_index);
        sample.pts_us = pts_us;
        report.timeline.push_back(sample);
    }

    normalize_quality_timeline_order(report);

    REQUIRE(report.timeline.size() == 4);
    CHECK(report.timeline[0].pts_us == 1'000);
    CHECK(report.timeline[0].decoded_frame_index == 11);
    CHECK(report.timeline[1].pts_us == 1'000);
    CHECK(report.timeline[1].decoded_frame_index == 12);
    CHECK(report.timeline[2].pts_us == 2'000);
    CHECK(report.timeline[3].pts_us == 3'000);
    for (size_t index = 0; index < report.timeline.size(); ++index) {
        CHECK(report.timeline[index].sample_index == index);
    }
}

TEST_CASE("quality event aggregation emits only robust relative outliers",
          "[analysis][quality][event]") {
    using namespace vr::analysis::quality;
    QualityReport report;
    report.sample_interval_us = 1'000'000;
    const std::array<double, 8> scores{
        0.10, 0.10, 0.11, 0.09, 0.10, 0.10, 0.80, 0.80};
    for (size_t index = 0; index < scores.size(); ++index) {
        FrameQualitySample sample;
        sample.sample_index = index;
        sample.pts_us = static_cast<int64_t>(index) * 1'000'000;
        sample.blockiness = scores[index];
        report.timeline.push_back(sample);
    }

    QualityEventAggregationOptions options;
    options.metric_mask = QualityMetricBlockiness;
    const auto events = aggregate_quality_events(report, options);
    REQUIRE(events.size() == 1);
    CHECK(events[0].metric == "blockiness");
    CHECK(
        events[0].classification ==
        QualityEventClassification::RelativeOutlier);
    CHECK(events[0].start_sample_index == 6);
    CHECK(events[0].end_sample_index == 7);
    CHECK(events[0].evidence_sample_count == 2);
    CHECK(events[0].peak_score == Catch::Approx(0.80));
    CHECK_FALSE(events[0].has_spatial_region);
    CHECK(
        events[0].threshold.kind ==
        QualityEventThresholdKind::RobustRelative);
    CHECK(events[0].threshold.median == Catch::Approx(0.10));

    for (auto& sample : report.timeline) {
        sample.blockiness = 0.25;
    }
    CHECK(aggregate_quality_events(report, options).empty());
}

TEST_CASE("quality event aggregation preserves precise banding evidence",
          "[analysis][quality][event][region]") {
    using namespace vr::analysis::quality;
    QualityReport report;
    report.sample_interval_us = 1'000'000;
    for (uint64_t index = 0; index < 3; ++index) {
        FrameQualitySample sample;
        sample.sample_index = index;
        sample.pts_us = static_cast<int64_t>(index) * 1'000'000;
        sample.banding = 0.20 + 0.05 * index;
        QualitySpatialRegion region;
        region.metric = "banding";
        region.score = index == 1 ? 0.90 : 0.60;
        region.detection_threshold = 0.40;
        region.x = index < 2 ? static_cast<int>(16 + index * 4) : 160;
        region.y = index < 2 ? 16 : 120;
        region.width = 48;
        region.height = 48;
        region.tile_count = 9;
        region.tile_span_columns = 3;
        region.tile_span_rows = 3;
        region.fill_ratio = 1.0;
        sample.spatial_regions.push_back(region);
        if (index < 2) {
            QualitySpatialRegion second = region;
            second.score = index == 1 ? 0.80 : 0.70;
            second.x = static_cast<int>(200 + index * 4);
            second.y = 16;
            sample.spatial_regions.push_back(second);
        }
        report.timeline.push_back(sample);
    }

    QualityEventAggregationOptions options;
    options.metric_mask = QualityMetricBanding;
    const auto events = aggregate_quality_events(report, options);
    REQUIRE(events.size() == 3);
    CHECK(
        events[0].classification ==
        QualityEventClassification::SpatialCandidate);
    CHECK(events[0].start_sample_index == 0);
    CHECK(events[0].end_sample_index == 1);
    CHECK(events[0].peak_sample_index == 1);
    CHECK(events[0].evidence_sample_count == 2);
    REQUIRE(events[0].has_spatial_region);
    CHECK(events[0].spatial_region.x == 20);
    CHECK(events[0].spatial_region.score == Catch::Approx(0.90));
    CHECK(
        events[0].threshold.kind ==
        QualityEventThresholdKind::SpatialDetection);
    CHECK(events[1].start_sample_index == 0);
    CHECK(events[1].end_sample_index == 1);
    CHECK(events[1].peak_sample_index == 1);
    REQUIRE(events[1].has_spatial_region);
    CHECK(events[1].spatial_region.x == 204);
    CHECK(events[1].spatial_region.score == Catch::Approx(0.80));
    CHECK(events[2].start_sample_index == 2);
    CHECK(events[2].end_sample_index == 2);

    options.include_spatial_regions = false;
    CHECK(aggregate_quality_events(report, options).empty());
}

TEST_CASE("quality event aggregation tracks gradually moving regions",
          "[analysis][quality][event][region]") {
    using namespace vr::analysis::quality;
    QualityReport report;
    report.sample_interval_us = 1'000'000;
    for (uint64_t index = 0; index < 3; ++index) {
        FrameQualitySample sample;
        sample.sample_index = index;
        sample.pts_us = static_cast<int64_t>(index) * 1'000'000;
        sample.banding = 0.20;
        QualitySpatialRegion region;
        region.metric = "banding";
        region.score = 0.90 - 0.10 * index;
        region.detection_threshold = 0.40;
        region.x = static_cast<int>(index) * 30;
        region.y = 16;
        region.width = 48;
        region.height = 48;
        region.tile_count = 9;
        region.tile_span_columns = 3;
        region.tile_span_rows = 3;
        region.fill_ratio = 1.0;
        sample.spatial_regions.push_back(region);
        report.timeline.push_back(sample);
    }

    QualityEventAggregationOptions options;
    options.metric_mask = QualityMetricBanding;
    const auto events = aggregate_quality_events(report, options);
    REQUIRE(events.size() == 1);
    CHECK(events[0].start_sample_index == 0);
    CHECK(events[0].end_sample_index == 2);
    CHECK(events[0].peak_sample_index == 0);
    CHECK(events[0].evidence_sample_count == 3);
    REQUIRE(events[0].has_spatial_region);
    CHECK(events[0].spatial_region.x == 0);
}

TEST_CASE("quality metrics reject invalid luma planes", "[analysis][quality]") {
    LumaPlaneView invalid;
    REQUIRE_FALSE(vr::analysis::quality::is_valid_luma_plane(invalid));
    REQUIRE(vr::analysis::quality::measure_blockiness(invalid) == 0.0);
    REQUIRE(vr::analysis::quality::measure_banding_proxy(invalid) == 0.0);
    REQUIRE(
        vr::analysis::quality::measure_banding_with_regions(invalid)
            .regions.empty());
    REQUIRE(vr::analysis::quality::measure_blur_proxy(invalid) == 0.0);
    REQUIRE(vr::analysis::quality::measure_noise_proxy(invalid) == 0.0);
    LumaTemporalSignature signature;
    REQUIRE_FALSE(
        vr::analysis::quality::make_temporal_signature(
            invalid, signature));
}

TEST_CASE("quality CPU dispatch matches scalar reference",
          "[analysis][quality][simd]") {
    constexpr int width = 193;
    constexpr int height = 109;
    std::vector<uint8_t> luma(
        static_cast<size_t>(width * height));
    uint32_t state = 0x12345678u;
    for (uint8_t& sample : luma) {
        state = state * 1664525u + 1013904223u;
        sample = static_cast<uint8_t>(state >> 24);
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 8; x < width; x += 8) {
            luma[static_cast<size_t>(y * width + x)] =
                static_cast<uint8_t>(
                    std::min(255,
                             static_cast<int>(
                                 luma[static_cast<size_t>(
                                     y * width + x)]) +
                                 24));
        }
    }
    const LumaPlaneView view{
        luma.data(), width, height, width, 1, 0, 8, 0};
    REQUIRE(
        vr::analysis::quality::measure_blockiness(view) ==
        Catch::Approx(
            vr::analysis::quality::measure_blockiness(
                view,
                vr::analysis::quality::QualityCpuMode::Scalar))
            .margin(1e-12));
    REQUIRE(
        vr::analysis::quality::measure_banding_proxy(view) ==
        Catch::Approx(
            vr::analysis::quality::measure_banding_proxy(
                view,
                vr::analysis::quality::QualityCpuMode::Scalar))
            .margin(1e-12));
    REQUIRE(
        vr::analysis::quality::measure_blur_proxy(view) ==
        Catch::Approx(
            vr::analysis::quality::measure_blur_proxy(
                view,
                vr::analysis::quality::QualityCpuMode::Scalar))
            .margin(1e-12));
    REQUIRE(
        vr::analysis::quality::measure_noise_proxy(view) ==
        Catch::Approx(
            vr::analysis::quality::measure_noise_proxy(
                view,
                vr::analysis::quality::QualityCpuMode::Scalar))
            .margin(1e-12));

    constexpr int padded_stride = width + 17;
    std::vector<uint8_t> padded(
        static_cast<size_t>(padded_stride * height), 0xa5);
    for (int y = 0; y < height; ++y) {
        std::copy_n(
            luma.data() + static_cast<size_t>(y * width),
            width,
            padded.data() +
                static_cast<size_t>(y * padded_stride));
    }
    const LumaPlaneView padded_view{
        padded.data(),
        width,
        height,
        padded_stride,
        1,
        0,
        8,
        0,
    };
    REQUIRE(
        vr::analysis::quality::measure_blockiness(padded_view) ==
        Catch::Approx(
            vr::analysis::quality::measure_blockiness(
                padded_view,
                vr::analysis::quality::QualityCpuMode::Scalar))
            .margin(1e-12));
    REQUIRE(
        vr::analysis::quality::measure_banding_proxy(padded_view) ==
        Catch::Approx(
            vr::analysis::quality::measure_banding_proxy(
                padded_view,
                vr::analysis::quality::QualityCpuMode::Scalar))
            .margin(1e-12));
    REQUIRE(
        vr::analysis::quality::measure_blur_proxy(padded_view) ==
        Catch::Approx(
            vr::analysis::quality::measure_blur_proxy(
                padded_view,
                vr::analysis::quality::QualityCpuMode::Scalar))
            .margin(1e-12));
    REQUIRE(
        vr::analysis::quality::measure_noise_proxy(padded_view) ==
        Catch::Approx(
            vr::analysis::quality::measure_noise_proxy(
                padded_view,
                vr::analysis::quality::QualityCpuMode::Scalar))
            .margin(1e-12));

    constexpr int padded_u16_stride = width * 2 + 22;
    std::vector<uint8_t> packed10(
        static_cast<size_t>(padded_u16_stride) * height, 0x5a);
    std::vector<uint8_t> p010(
        static_cast<size_t>(padded_u16_stride) * height, 0xa5);
    std::vector<uint8_t> packed12(
        static_cast<size_t>(padded_u16_stride) * height, 0x3c);
    std::vector<uint8_t> packed16(
        static_cast<size_t>(padded_u16_stride) * height, 0xc3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint16_t value10 =
                static_cast<uint16_t>(
                    luma[static_cast<size_t>(y * width + x)]) *
                4u;
            const uint16_t stored_p010 =
                static_cast<uint16_t>(value10 << 6);
            const uint16_t value12 =
                static_cast<uint16_t>(
                    luma[static_cast<size_t>(y * width + x)]) *
                16u;
            const uint16_t value16 =
                static_cast<uint16_t>(
                    luma[static_cast<size_t>(y * width + x)]) *
                257u;
            std::memcpy(
                packed10.data() +
                    static_cast<size_t>(y * padded_u16_stride + x * 2),
                &value10,
                sizeof(value10));
            std::memcpy(
                p010.data() +
                    static_cast<size_t>(y * padded_u16_stride + x * 2),
                &stored_p010,
                sizeof(stored_p010));
            std::memcpy(
                packed12.data() +
                    static_cast<size_t>(y * padded_u16_stride + x * 2),
                &value12,
                sizeof(value12));
            std::memcpy(
                packed16.data() +
                    static_cast<size_t>(y * padded_u16_stride + x * 2),
                &value16,
                sizeof(value16));
        }
    }
    const LumaPlaneView packed10_view{
        packed10.data(),
        width,
        height,
        padded_u16_stride,
        2,
        0,
        10,
        0,
    };
    LumaPlaneView p010_view = packed10_view;
    p010_view.data = p010.data();
    p010_view.sample_shift = 6;
    LumaPlaneView packed12_view = packed10_view;
    packed12_view.data = packed12.data();
    packed12_view.bit_depth = 12;
    LumaPlaneView packed16_view = packed10_view;
    packed16_view.data = packed16.data();
    packed16_view.bit_depth = 16;
    for (const LumaPlaneView& hdr_view :
         {packed10_view,
          p010_view,
          packed12_view,
          packed16_view}) {
        REQUIRE(
            vr::analysis::quality::measure_blockiness(hdr_view) ==
            Catch::Approx(
                vr::analysis::quality::measure_blockiness(
                    hdr_view,
                    vr::analysis::quality::QualityCpuMode::Scalar))
                .margin(1e-12));
        REQUIRE(
            vr::analysis::quality::measure_banding_proxy(hdr_view) ==
            Catch::Approx(
                vr::analysis::quality::measure_banding_proxy(
                    hdr_view,
                    vr::analysis::quality::QualityCpuMode::Scalar))
                .margin(1e-12));
        REQUIRE(
            vr::analysis::quality::measure_blur_proxy(hdr_view) ==
            Catch::Approx(
                vr::analysis::quality::measure_blur_proxy(
                    hdr_view,
                    vr::analysis::quality::QualityCpuMode::Scalar))
                .margin(1e-12));
        REQUIRE(
            vr::analysis::quality::measure_noise_proxy(hdr_view) ==
            Catch::Approx(
                vr::analysis::quality::measure_noise_proxy(
                    hdr_view,
                    vr::analysis::quality::QualityCpuMode::Scalar))
                .margin(1e-12));
    }
}

TEST_CASE("blockiness proxy responds to block-aligned discontinuities",
          "[analysis][quality]") {
    constexpr int width = 64;
    constexpr int height = 64;
    std::vector<uint8_t> flat(width * height, 96);
    std::vector<uint8_t> gradient(width * height);
    std::vector<uint8_t> blocked(width * height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            gradient[y * width + x] =
                static_cast<uint8_t>(32 + x + y);
            blocked[y * width + x] =
                ((x / 8 + y / 8) % 2 == 0) ? 48 : 208;
        }
    }

    const double flat_score =
        vr::analysis::quality::measure_blockiness(
            make_u8_view(flat, width, height));
    const double gradient_score =
        vr::analysis::quality::measure_blockiness(
            make_u8_view(gradient, width, height));
    const double blocked_score =
        vr::analysis::quality::measure_blockiness(
            make_u8_view(blocked, width, height));

    REQUIRE(flat_score == 0.0);
    REQUIRE(gradient_score < 0.02);
    REQUIRE(blocked_score > 0.60);
}

TEST_CASE("banding proxy separates quantized plateaus from a smooth ramp",
          "[analysis][quality]") {
    constexpr int width = 64;
    constexpr int height = 64;
    std::vector<uint8_t> smooth(width * height);
    std::vector<uint8_t> banded(width * height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            smooth[y * width + x] =
                static_cast<uint8_t>(64 + x);
            banded[y * width + x] =
                static_cast<uint8_t>(64 + (x / 4) * 2);
        }
    }

    const double smooth_score =
        vr::analysis::quality::measure_banding_proxy(
            make_u8_view(smooth, width, height));
    const double banded_score =
        vr::analysis::quality::measure_banding_proxy(
            make_u8_view(banded, width, height));

    REQUIRE(smooth_score < 0.01);
    REQUIRE(banded_score > 0.15);
    REQUIRE(banded_score > smooth_score + 0.10);
}

TEST_CASE("banding spatial output merges adjacent high-score tiles",
          "[analysis][quality][spatial]") {
    constexpr int width = 96;
    constexpr int height = 64;
    std::vector<uint8_t> luma(width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t value = static_cast<uint8_t>(48 + (x % 16));
            if (x >= 32 && x < 64 && y >= 16 && y < 48) {
                value = static_cast<uint8_t>(96 + ((x - 32) % 16) / 4 * 2);
            }
            luma[static_cast<size_t>(y * width + x)] = value;
        }
    }

    const LumaPlaneView view = make_u8_view(luma, width, height);
    const auto scalar = vr::analysis::quality::measure_banding_with_regions(
        view, vr::analysis::quality::QualityCpuMode::Scalar);
    const auto automatic =
        vr::analysis::quality::measure_banding_with_regions(view);
    const auto score_only =
        vr::analysis::quality::measure_banding_with_regions(
            view,
            vr::analysis::quality::QualityCpuMode::Auto,
            false);

    REQUIRE(scalar.score == Catch::Approx(automatic.score).margin(1e-12));
    REQUIRE(score_only.score ==
            Catch::Approx(automatic.score).margin(1e-12));
    REQUIRE(score_only.regions.empty());
    REQUIRE(scalar.regions.size() == 1);
    REQUIRE(automatic.regions.size() == scalar.regions.size());
    const auto& region = automatic.regions.front();
    REQUIRE(region.metric == "banding");
    REQUIRE(region.score >= region.detection_threshold);
    REQUIRE(region.x == 32);
    REQUIRE(region.y == 16);
    REQUIRE(region.width == 32);
    REQUIRE(region.height == 32);
    REQUIRE(region.tile_count == 4);
    REQUIRE(region.tile_span_columns == 2);
    REQUIRE(region.tile_span_rows == 2);
    REQUIRE(region.fill_ratio == Catch::Approx(1.0));
}

TEST_CASE("banding spatial output rejects single-tile-thick strips",
          "[analysis][quality][spatial]") {
    constexpr int width = 128;
    constexpr int height = 64;
    std::vector<uint8_t> luma(width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t value = static_cast<uint8_t>(48 + (x % 16));
            if (x >= 16 && x < 112 && y >= 16 && y < 32) {
                value = static_cast<uint8_t>(96 + ((x - 16) % 16) / 4 * 2);
            }
            luma[static_cast<size_t>(y * width + x)] = value;
        }
    }

    const auto measurement =
        vr::analysis::quality::measure_banding_with_regions(
            make_u8_view(luma, width, height));

    REQUIRE(measurement.score > 0.04);
    REQUIRE(measurement.regions.empty());
}

TEST_CASE("banding spatial output rejects sparse connected components",
          "[analysis][quality][spatial]") {
    constexpr int width = 128;
    constexpr int height = 128;
    constexpr std::array<std::array<int, 2>, 7> sparse_tiles{{
        {{1, 1}}, {{2, 1}}, {{2, 2}}, {{3, 2}},
        {{3, 3}}, {{4, 3}}, {{4, 4}},
    }};
    std::vector<uint8_t> luma(width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bool in_sparse_component = false;
            for (const auto& tile : sparse_tiles) {
                if (x / 16 == tile[0] && y / 16 == tile[1]) {
                    in_sparse_component = true;
                    break;
                }
            }
            luma[static_cast<size_t>(y * width + x)] =
                in_sparse_component
                    ? static_cast<uint8_t>(96 + (x % 16) / 4 * 2)
                    : static_cast<uint8_t>(48 + (x % 16));
        }
    }

    const auto measurement =
        vr::analysis::quality::measure_banding_with_regions(
            make_u8_view(luma, width, height));

    REQUIRE(measurement.score > 0.02);
    REQUIRE(measurement.regions.empty());
}

TEST_CASE("quality metrics preserve behavior for little-endian 10-bit luma",
          "[analysis][quality]") {
    constexpr int width = 64;
    constexpr int height = 64;
    std::vector<uint8_t> pixels8(width * height);
    std::vector<uint16_t> pixels10(width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t value =
                ((x / 8 + y / 8) % 2 == 0) ? 48 : 208;
            pixels8[y * width + x] = value;
            pixels10[y * width + x] =
                static_cast<uint16_t>(value) << 2;
        }
    }

    LumaPlaneView view10;
    view10.data = reinterpret_cast<const uint8_t*>(pixels10.data());
    view10.width = width;
    view10.height = height;
    view10.stride_bytes = width * 2;
    view10.sample_step_bytes = 2;
    view10.bit_depth = 10;

    const double score8 =
        vr::analysis::quality::measure_blockiness(
            make_u8_view(pixels8, width, height));
    const double score10 =
        vr::analysis::quality::measure_blockiness(view10);
    REQUIRE(score10 == Catch::Approx(score8).margin(0.002));

    REQUIRE(
        vr::analysis::quality::measure_blur_proxy(view10) ==
        Catch::Approx(vr::analysis::quality::measure_blur_proxy(
                          make_u8_view(pixels8, width, height)))
            .margin(0.002));
    REQUIRE(
        vr::analysis::quality::measure_noise_proxy(view10) ==
        Catch::Approx(vr::analysis::quality::measure_noise_proxy(
                          make_u8_view(pixels8, width, height)))
            .margin(0.004));

    LumaTemporalSignature signature8;
    LumaTemporalSignature signature10;
    REQUIRE(vr::analysis::quality::make_temporal_signature(
        make_u8_view(pixels8, width, height), signature8));
    REQUIRE(vr::analysis::quality::make_temporal_signature(
        view10, signature10));
    REQUIRE(signature10.mean_luma ==
            Catch::Approx(signature8.mean_luma).margin(0.51));
}

TEST_CASE("packed 8-bit luma matches planar metrics at either byte offset",
          "[analysis][quality]") {
    constexpr int width = 64;
    constexpr int height = 64;
    constexpr int packed_stride = width * 2 + 8;
    std::vector<uint8_t> planar(width * height);
    std::vector<uint8_t> yuyv(
        static_cast<size_t>(packed_stride * height), 0x35);
    std::vector<uint8_t> uyvy(
        static_cast<size_t>(packed_stride * height), 0xca);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t value = static_cast<uint8_t>(
                24 + ((x * 13 + y * 7 + (x / 8) * 19) % 208));
            planar[static_cast<size_t>(y * width + x)] = value;
            yuyv[static_cast<size_t>(
                y * packed_stride + x * 2)] = value;
            uyvy[static_cast<size_t>(
                y * packed_stride + x * 2 + 1)] = value;
        }
    }

    const LumaPlaneView planar_view =
        make_u8_view(planar, width, height);
    const LumaPlaneView yuyv_view{
        yuyv.data(), width, height, packed_stride, 2, 0, 8, 0};
    const LumaPlaneView uyvy_view{
        uyvy.data(), width, height, packed_stride, 2, 1, 8, 0};

    REQUIRE(vr::analysis::quality::is_valid_luma_plane(yuyv_view));
    REQUIRE(vr::analysis::quality::is_valid_luma_plane(uyvy_view));
    for (const LumaPlaneView& packed_view : {yuyv_view, uyvy_view}) {
        REQUIRE(
            vr::analysis::quality::measure_blockiness(packed_view) ==
            Catch::Approx(
                vr::analysis::quality::measure_blockiness(planar_view))
                .margin(1e-12));
        REQUIRE(
            vr::analysis::quality::measure_banding_proxy(packed_view) ==
            Catch::Approx(
                vr::analysis::quality::measure_banding_proxy(planar_view))
                .margin(1e-12));
        REQUIRE(
            vr::analysis::quality::measure_blur_proxy(packed_view) ==
            Catch::Approx(
                vr::analysis::quality::measure_blur_proxy(planar_view))
                .margin(1e-12));
        REQUIRE(
            vr::analysis::quality::measure_noise_proxy(packed_view) ==
            Catch::Approx(
                vr::analysis::quality::measure_noise_proxy(planar_view))
                .margin(1e-12));

        LumaTemporalSignature packed_signature;
        LumaTemporalSignature planar_signature;
        REQUIRE(vr::analysis::quality::make_temporal_signature(
            packed_view, packed_signature));
        REQUIRE(vr::analysis::quality::make_temporal_signature(
            planar_view, planar_signature));
        REQUIRE(packed_signature.mean_luma ==
                Catch::Approx(planar_signature.mean_luma)
                    .margin(1e-12));
        REQUIRE(packed_signature.tile_means ==
                planar_signature.tile_means);
    }

    LumaPlaneView invalid_offset = uyvy_view;
    invalid_offset.bit_depth = 10;
    REQUIRE_FALSE(
        vr::analysis::quality::is_valid_luma_plane(invalid_offset));
}

TEST_CASE("blur proxy increases after deterministic spatial smoothing",
          "[analysis][quality]") {
    constexpr int width = 64;
    constexpr int height = 64;
    std::vector<uint8_t> sharp(width * height);
    std::vector<uint8_t> blurred(width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            sharp[y * width + x] =
                (x % 16 < 8) ? 32 : 224;
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sum = 0;
            for (int offset = -3; offset <= 3; ++offset) {
                sum += sharp[y * width +
                             std::clamp(x + offset, 0, width - 1)];
            }
            blurred[y * width + x] =
                static_cast<uint8_t>(sum / 7);
        }
    }

    const double sharp_score =
        vr::analysis::quality::measure_blur_proxy(
            make_u8_view(sharp, width, height));
    const double blurred_score =
        vr::analysis::quality::measure_blur_proxy(
            make_u8_view(blurred, width, height));

    REQUIRE(sharp_score < 0.40);
    REQUIRE(blurred_score > sharp_score + 0.25);
}

TEST_CASE("noise proxy responds to high-frequency residuals in flat content",
          "[analysis][quality]") {
    constexpr int width = 64;
    constexpr int height = 64;
    std::vector<uint8_t> clean(width * height, 128);
    std::vector<uint8_t> noisy(width * height);
    uint32_t state = 0x12345678u;
    for (uint8_t& value : noisy) {
        state = state * 1664525u + 1013904223u;
        const int noise = static_cast<int>((state >> 24) % 33) - 16;
        value = static_cast<uint8_t>(128 + noise);
    }

    const double clean_score =
        vr::analysis::quality::measure_noise_proxy(
            make_u8_view(clean, width, height));
    const double noisy_score =
        vr::analysis::quality::measure_noise_proxy(
            make_u8_view(noisy, width, height));

    REQUIRE(clean_score == 0.0);
    REQUIRE(noisy_score > 0.20);
}

TEST_CASE("flicker proxy detects alternating exposure but ignores linear fades",
          "[analysis][quality]") {
    auto signature = [](double value) {
        LumaTemporalSignature result;
        result.mean_luma = value;
        result.tile_means.assign(16 * 9, value);
        return result;
    };

    const double stable =
        vr::analysis::quality::measure_flicker_proxy(
            signature(100.0), signature(100.0), signature(100.0));
    const double linear_fade =
        vr::analysis::quality::measure_flicker_proxy(
            signature(100.0), signature(110.0), signature(120.0));
    const double alternating =
        vr::analysis::quality::measure_flicker_proxy(
            signature(100.0), signature(120.0), signature(100.0));
    const double scene_cut =
        vr::analysis::quality::measure_flicker_proxy(
            signature(100.0), signature(220.0), signature(100.0));

    REQUIRE(stable == 0.0);
    REQUIRE(linear_fade == Catch::Approx(0.0));
    REQUIRE(alternating > 0.50);
    REQUIRE(scene_cut < 0.0);
}

TEST_CASE("quality tile metrics share a balanced row-major grid",
          "[analysis][quality][tiles]") {
    using namespace vr::analysis::quality;
    constexpr int width = 130;
    constexpr int height = 70;
    std::vector<uint8_t> luma(
        static_cast<size_t>(width * height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            luma[static_cast<size_t>(y * width + x)] =
                static_cast<uint8_t>(
                    (x * 5 + y * 3 + ((x / 8 + y / 8) % 2) * 47) &
                    0xff);
        }
    }
    const LumaPlaneView view = make_u8_view(luma, width, height);
    constexpr std::array<QualityTileMetric, 4> metrics{
        QualityTileMetric::Blockiness,
        QualityTileMetric::Banding,
        QualityTileMetric::Blur,
        QualityTileMetric::Noise,
    };
    for (const QualityTileMetric metric : metrics) {
        const QualityTileMeasurement measurement =
            measure_quality_tiles(
                view, metric, QualityCpuMode::Scalar);
        REQUIRE(measurement.columns == 3);
        REQUIRE(measurement.rows == 2);
        REQUIRE(measurement.scores.size() == 6);
        for (const double score : measurement.scores) {
            REQUIRE(score >= 0.0);
            REQUIRE(score <= 1.0);
        }
    }
}

TEST_CASE("flicker tile metrics preserve local temporal curvature",
          "[analysis][quality][tiles]") {
    using namespace vr::analysis::quality;
    constexpr int width = 128;
    constexpr int height = 64;
    auto frame = [](uint8_t left, uint8_t right) {
        std::vector<uint8_t> pixels(
            static_cast<size_t>(width * height));
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                pixels[static_cast<size_t>(y * width + x)] =
                    x < width / 2 ? left : right;
            }
        }
        return pixels;
    };
    const auto first = frame(100, 100);
    const auto middle = frame(120, 100);
    const auto last = frame(100, 100);
    LumaTemporalSignature first_signature;
    LumaTemporalSignature middle_signature;
    LumaTemporalSignature last_signature;
    REQUIRE(make_temporal_tile_signature(
        make_u8_view(first, width, height), first_signature));
    REQUIRE(make_temporal_tile_signature(
        make_u8_view(middle, width, height), middle_signature));
    REQUIRE(make_temporal_tile_signature(
        make_u8_view(last, width, height), last_signature));

    std::vector<double> tile_scores;
    const double frame_score = measure_flicker_proxy_with_tiles(
        first_signature, middle_signature, last_signature, tile_scores);
    REQUIRE(first_signature.columns == 2);
    REQUIRE(first_signature.rows == 1);
    REQUIRE(tile_scores.size() == 2);
    REQUIRE(frame_score > 0.0);
    REQUIRE(tile_scores[0] > 0.50);
    REQUIRE(tile_scores[1] == Catch::Approx(0.0));
}

TEST_CASE("wgpu quality backend matches CPU reference for 8 and 10 bit luma",
          "[analysis][quality][wgpu]") {
    std::string error;
    auto backend =
        vr::analysis::quality::WgpuQualityBackend::create(error);
    if (!backend) {
        SKIP("wgpu quality backend unavailable: " + error);
    }

    constexpr int width = 64;
    constexpr int height = 64;
    std::vector<uint8_t> pixels8(width * height);
    std::vector<uint16_t> pixels10(width * height);
    std::vector<uint16_t> pixels_p010(width * height);
    uint32_t state = 0x9e3779b9u;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            state = state * 1664525u + 1013904223u;
            const int noise = static_cast<int>((state >> 27) & 7u) - 3;
            const int blocked =
                ((x / 8 + y / 8) % 2 == 0) ? 64 : 176;
            const uint8_t value = static_cast<uint8_t>(
                std::clamp(blocked + x / 4 + noise, 0, 255));
            pixels8[y * width + x] = value;
            pixels10[y * width + x] =
                static_cast<uint16_t>(value) << 2;
            pixels_p010[y * width + x] =
                static_cast<uint16_t>(value) << 8;
        }
    }

    const LumaPlaneView view8 =
        make_u8_view(pixels8, width, height);
    LumaPlaneView view10;
    view10.data =
        reinterpret_cast<const uint8_t*>(pixels10.data());
    view10.width = width;
    view10.height = height;
    view10.stride_bytes = width * 2;
    view10.sample_step_bytes = 2;
    view10.bit_depth = 10;

    LumaPlaneView view_p010 = view10;
    view_p010.data =
        reinterpret_cast<const uint8_t*>(pixels_p010.data());
    view_p010.sample_shift = 6;

    constexpr int padded_stride = width + 8;
    std::vector<uint8_t> padded(
        static_cast<size_t>(padded_stride) * height, 0);
    for (int y = 0; y < height; ++y) {
        std::copy_n(
            pixels8.data() + y * width,
            width,
            padded.data() + y * padded_stride);
    }
    LumaPlaneView padded_view = view8;
    padded_view.data = padded.data();
    padded_view.stride_bytes = padded_stride;

    for (const LumaPlaneView& view :
         {view8, view10, view_p010, padded_view}) {
        vr::analysis::quality::WgpuQualityScores gpu;
        REQUIRE(backend->score_plane(view, gpu, error));
        REQUIRE(gpu.blockiness ==
                Catch::Approx(
                    vr::analysis::quality::measure_blockiness(view))
                    .margin(1e-12));
        REQUIRE(gpu.banding ==
                Catch::Approx(
                    vr::analysis::quality::measure_banding_proxy(view))
                    .margin(1e-12));
        REQUIRE(gpu.blur ==
                Catch::Approx(
                    vr::analysis::quality::measure_blur_proxy(view))
                    .margin(1e-12));
        REQUIRE(gpu.noise ==
                Catch::Approx(
                    vr::analysis::quality::measure_noise_proxy(view))
                    .margin(1e-12));
        REQUIRE(gpu.total_ms > 0.0);
    }
}

TEST_CASE("wgpu quality backend keeps three submissions in flight",
          "[analysis][quality][wgpu]") {
    std::string error;
    auto backend =
        vr::analysis::quality::WgpuQualityBackend::create(error);
    if (!backend) {
        SKIP("wgpu quality backend unavailable: " + error);
    }
    REQUIRE(backend->max_in_flight() == 3);

    constexpr int width = 64;
    constexpr int height = 64;
    std::vector<uint8_t> pixels(width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            pixels[y * width + x] =
                static_cast<uint8_t>((x * 3 + y * 5) & 0xff);
        }
    }
    const LumaPlaneView view =
        make_u8_view(pixels, width, height);

    std::vector<vr::analysis::quality::WgpuQualityBackend::Ticket> tickets;
    for (uint32_t index = 0; index < backend->max_in_flight(); ++index) {
        vr::analysis::quality::WgpuQualityBackend::Ticket ticket = 0;
        REQUIRE(backend->submit_plane(view, ticket, error));
        REQUIRE(ticket != 0);
        tickets.push_back(ticket);
    }
    vr::analysis::quality::WgpuQualityBackend::Ticket overflow = 0;
    REQUIRE_FALSE(backend->submit_plane(view, overflow, error));
    REQUIRE(error.find("in-flight queue is full") != std::string::npos);

    for (const auto ticket : tickets) {
        vr::analysis::quality::WgpuQualityScores gpu;
        REQUIRE(backend->collect_plane(ticket, gpu, error));
        REQUIRE(gpu.blockiness ==
                Catch::Approx(
                    vr::analysis::quality::measure_blockiness(view))
                    .margin(1e-12));
        REQUIRE(gpu.total_ms > 0.0);
        REQUIRE(gpu.latency_ms >= gpu.total_ms);
    }
}

TEST_CASE("quality distribution reports deterministic interpolated percentiles",
          "[analysis][quality]") {
    const auto summary =
        vr::analysis::quality::summarize_distribution(
            {5.0, 1.0, 3.0, 2.0, 4.0});

    REQUIRE(summary.count == 5);
    REQUIRE(summary.mean == Catch::Approx(3.0));
    REQUIRE(summary.p10 == Catch::Approx(1.4));
    REQUIRE(summary.p50 == Catch::Approx(3.0));
    REQUIRE(summary.p90 == Catch::Approx(4.6));
    REQUIRE(summary.p95 == Catch::Approx(4.8));
    REQUIRE(summary.maximum == Catch::Approx(5.0));
}
