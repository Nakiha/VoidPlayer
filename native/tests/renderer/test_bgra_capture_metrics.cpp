#include <catch2/catch_test_macros.hpp>

#include "renderer/capture/bgra_capture_metrics.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

void set_pixel(std::vector<uint8_t>& pixels,
               int width,
               int x,
               int y,
               uint8_t value) {
    const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
    pixels[offset + 0] = value;
    pixels[offset + 1] = value;
    pixels[offset + 2] = value;
    pixels[offset + 3] = 255;
}

} // namespace

TEST_CASE("BGRA overlay line style detects black-white-black contrast lines",
          "[capture][overlay_line_style]") {
    constexpr int kWidth = 12;
    constexpr int kHeight = 12;
    std::vector<uint8_t> pixels(kWidth * kHeight * 4, 128);

    for (int y = 2; y < kHeight - 2; ++y) {
        set_pixel(pixels, kWidth, 4, y, 0);
        set_pixel(pixels, kWidth, 5, y, 255);
        set_pixel(pixels, kWidth, 6, y, 0);
    }

    const auto metrics = vr::measure_bgra_overlay_line_style(
        pixels.data(), kWidth, kHeight, kWidth * 4);
    REQUIRE(metrics.paired_centers == 8);
    REQUIRE(metrics.weak_white_centers == 0);
    REQUIRE(metrics.black_only_centers == 0);
}

TEST_CASE("BGRA overlay line style distinguishes weak white and black-only lines",
          "[capture][overlay_line_style]") {
    constexpr int kWidth = 16;
    constexpr int kHeight = 12;
    std::vector<uint8_t> pixels(kWidth * kHeight * 4, 128);

    for (int y = 2; y < kHeight - 2; ++y) {
        set_pixel(pixels, kWidth, 4, y, 255);
        set_pixel(pixels, kWidth, 11, y, 0);
    }

    const auto metrics = vr::measure_bgra_overlay_line_style(
        pixels.data(), kWidth, kHeight, kWidth * 4);
    REQUIRE(metrics.paired_centers == 0);
    REQUIRE(metrics.weak_white_centers == 8);
    REQUIRE(metrics.black_only_centers == 8);
}
