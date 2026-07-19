#include "analysis/quality/quality_metrics.h"
#include "analysis/quality/quality_wgpu_backend.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

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

TEST_CASE("quality metrics reject invalid luma planes", "[analysis][quality]") {
    LumaPlaneView invalid;
    REQUIRE_FALSE(vr::analysis::quality::is_valid_luma_plane(invalid));
    REQUIRE(vr::analysis::quality::measure_blockiness(invalid) == 0.0);
    REQUIRE(vr::analysis::quality::measure_banding_proxy(invalid) == 0.0);
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
