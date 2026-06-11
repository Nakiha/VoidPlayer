#include "renderer/color/color_reference.h"

#include "renderer/color/color_strategy.h"
#include "renderer/frame/frame_storage.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

struct Case {
  const char* name = "";
  vr::ColorReferenceYuv sample;
  vr::ColorReferenceConfig config;
  vr::ColorReferenceRgb expected;
  double tolerance = 1e-6;
};

double abs_diff(double a, double b) {
  return std::abs(a - b);
}

bool close_rgb(const vr::ColorReferenceRgb& actual,
               const vr::ColorReferenceRgb& expected,
               double tolerance) {
  return abs_diff(actual.r, expected.r) <= tolerance &&
      abs_diff(actual.g, expected.g) <= tolerance &&
      abs_diff(actual.b, expected.b) <= tolerance;
}

void print_rgb(const char* label, const vr::ColorReferenceRgb& rgb) {
  std::fprintf(stderr,
               "%s=(%.9f, %.9f, %.9f)",
               label,
               rgb.r,
               rgb.g,
               rgb.b);
}

vr::ColorReferenceYuv limited8(int y, int u, int v) {
  return {
      vr::color_reference_unorm_to_float(static_cast<uint32_t>(y), 8),
      vr::color_reference_unorm_to_float(static_cast<uint32_t>(u), 8),
      vr::color_reference_unorm_to_float(static_cast<uint32_t>(v), 8),
  };
}

vr::ColorReferenceYuv limited_from_luma(double y_full) {
  return {
      (y_full * 219.0 + 16.0) / 255.0,
      128.0 / 255.0,
      128.0 / 255.0,
  };
}

vr::ColorReferenceConfig config(int matrix,
                                int transfer,
                                int primaries,
                                bool output_edr) {
  return {
      vr::VIDEO_COLOR_RANGE_LIMITED,
      matrix,
      transfer,
      primaries,
      output_edr,
  };
}

bool run_case(const Case& test_case) {
  const auto actual =
      vr::color_reference_sample_yuv(test_case.sample, test_case.config);
  if (close_rgb(actual, test_case.expected, test_case.tolerance)) {
    return true;
  }
  std::fprintf(stderr, "%s failed: ", test_case.name);
  print_rgb("actual", actual);
  std::fprintf(stderr, " ");
  print_rgb("expected", test_case.expected);
  std::fprintf(stderr, " tolerance=%.9f\n", test_case.tolerance);
  return false;
}

bool expect_between(const char* name, double value, double low, double high) {
  if (value >= low && value <= high) {
    return true;
  }
  std::fprintf(stderr,
               "%s failed: value=%.9f expected=[%.9f, %.9f]\n",
               name,
               value,
               low,
               high);
  return false;
}

} // namespace

int main() {
  const double sdr_white = vr::color_reference_srgb_to_linear(254.0 / 255.0);
  const double bt2020_p3_white_r = vr::kBT2020ToDisplayP3RFromR +
      vr::kBT2020ToDisplayP3RFromG + vr::kBT2020ToDisplayP3RFromB;
  const double bt2020_p3_white_g = vr::kBT2020ToDisplayP3GFromR +
      vr::kBT2020ToDisplayP3GFromG + vr::kBT2020ToDisplayP3GFromB;
  const double bt2020_p3_white_b = vr::kBT2020ToDisplayP3BFromR +
      vr::kBT2020ToDisplayP3BFromG + vr::kBT2020ToDisplayP3BFromB;
  const double pq_203_nits =
      vr::color_reference_pq_to_linear_nits(0.5806889) /
      vr::kHDRReferenceWhiteNits;

  const std::vector<Case> cases = {
      {
          "sdr limited black maps to edr black",
          limited8(16, 128, 128),
          config(vr::VIDEO_COLOR_MATRIX_BT709,
                 vr::VIDEO_COLOR_TRANSFER_SDR,
                 vr::VIDEO_COLOR_PRIMARIES_BT709,
                 true),
          {0.0, 0.0, 0.0},
          1e-9,
      },
      {
          "sdr limited white keeps edr below one",
          limited8(235, 128, 128),
          config(vr::VIDEO_COLOR_MATRIX_BT709,
                 vr::VIDEO_COLOR_TRANSFER_SDR,
                 vr::VIDEO_COLOR_PRIMARIES_BT709,
                 true),
          {sdr_white, sdr_white, sdr_white},
          1e-9,
      },
      {
          "hlg limited white maps to 4x edr",
          limited8(235, 128, 128),
          config(vr::VIDEO_COLOR_MATRIX_BT2020_NCL,
                 vr::VIDEO_COLOR_TRANSFER_HLG,
                 vr::VIDEO_COLOR_PRIMARIES_BT2020,
                 true),
          {
              bt2020_p3_white_r * vr::kHLGEDRHeadroomScale,
              bt2020_p3_white_g * vr::kHLGEDRHeadroomScale,
              bt2020_p3_white_b * vr::kHLGEDRHeadroomScale,
          },
          1e-5,
      },
      {
          "pq 203 nit code maps near edr one",
          limited_from_luma(0.5806889),
          config(vr::VIDEO_COLOR_MATRIX_BT2020_NCL,
                 vr::VIDEO_COLOR_TRANSFER_PQ,
                 vr::VIDEO_COLOR_PRIMARIES_BT2020,
                 true),
          {
              bt2020_p3_white_r * pq_203_nits,
              bt2020_p3_white_g * pq_203_nits,
              bt2020_p3_white_b * pq_203_nits,
          },
          5e-5,
      },
  };

  bool ok = true;
  for (const auto& test_case : cases) {
    ok = run_case(test_case) && ok;
  }

  const auto hlg_sdr = vr::color_reference_sample_yuv(
      limited8(235, 128, 128),
      config(vr::VIDEO_COLOR_MATRIX_BT2020_NCL,
             vr::VIDEO_COLOR_TRANSFER_HLG,
             vr::VIDEO_COLOR_PRIMARIES_BT2020,
             false));
  ok = expect_between("hlg sdr tone mapped white", hlg_sdr.r, 0.90, 0.91) && ok;
  ok = expect_between("sdr white edr headroom", sdr_white, 0.99, 1.0) && ok;
  ok = expect_between("pq 203 nit edr anchor", pq_203_nits, 0.99, 1.01) && ok;

  const auto bt2020_red = vr::color_reference_map_to_output(
      {1.0, 0.0, 0.0},
      {
          vr::VIDEO_COLOR_RANGE_FULL,
          vr::VIDEO_COLOR_MATRIX_BT709,
          vr::VIDEO_COLOR_TRANSFER_SDR,
          vr::VIDEO_COLOR_PRIMARIES_BT2020,
          true,
      });
  const bool bt2020_red_ok = close_rgb(
      bt2020_red,
      {
          vr::kBT2020ToDisplayP3RFromR,
          0.0,
          vr::kBT2020ToDisplayP3BFromR,
      },
      1e-6);
  ok = bt2020_red_ok && ok;
  if (!bt2020_red_ok) {
    std::fprintf(stderr, "bt2020 red primary transform failed: ");
    print_rgb("actual", bt2020_red);
    std::fprintf(stderr, "\n");
  }

  const auto bt709_red = vr::color_reference_map_to_output(
      {1.0, 0.0, 0.0},
      {
          vr::VIDEO_COLOR_RANGE_FULL,
          vr::VIDEO_COLOR_MATRIX_BT709,
          vr::VIDEO_COLOR_TRANSFER_SDR,
          vr::VIDEO_COLOR_PRIMARIES_BT709,
          true,
      });
  const bool bt709_red_ok = close_rgb(
      bt709_red,
      {
          vr::kBT709ToDisplayP3RFromR,
          vr::kBT709ToDisplayP3GFromR,
          vr::kBT709ToDisplayP3BFromR,
      },
      1e-6);
  ok = bt709_red_ok && ok;
  if (!bt709_red_ok) {
    std::fprintf(stderr, "bt709 red primary transform failed: ");
    print_rgb("actual", bt709_red);
    std::fprintf(stderr, "\n");
  }

  if (!ok) {
    return 1;
  }
  return 0;
}
