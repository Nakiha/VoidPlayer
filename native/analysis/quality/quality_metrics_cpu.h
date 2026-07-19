#pragma once

#include "analysis/quality/quality_metrics.h"

#include <array>
#include <cstdint>

namespace vr::analysis::quality::cpu {

struct BlockinessPeriodSums {
    uint64_t horizontal_boundary = 0;
    uint64_t horizontal_boundary_count = 0;
    uint64_t horizontal_interior = 0;
    uint64_t horizontal_interior_count = 0;
    uint64_t vertical_boundary = 0;
    uint64_t vertical_boundary_count = 0;
    uint64_t vertical_interior = 0;
    uint64_t vertical_interior_count = 0;
};

struct BlurDirectionSums {
    uint64_t original_times_five = 0;
    uint64_t lost_times_five = 0;
    uint64_t edge_count = 0;
};

struct BandingTileStats {
    std::array<uint64_t, 4> present{};
    uint64_t weak_contours = 0;
    uint64_t edge_count = 0;
    uint64_t gradient_sum = 0;
    int minimum = 255;
    int maximum = 0;
};

bool avx2_is_available();
bool blockiness_period_avx2_u8(const LumaPlaneView& plane,
                              int period,
                              BlockinessPeriodSums& sums);
bool blur_direction_avx2_u8(const LumaPlaneView& plane,
                            bool horizontal,
                            BlurDirectionSums& sums);
bool noise_tile_gradient_avx2_u8(const LumaPlaneView& plane,
                                 int tile_x,
                                 int tile_y,
                                 int tile_width,
                                 int tile_height,
                                 uint64_t& gradient_sum,
                                 uint64_t& gradient_count);
bool noise_tile_residual_avx2_u8(const LumaPlaneView& plane,
                                 int tile_x,
                                 int tile_y,
                                 int tile_width,
                                 int tile_height,
                                 uint64_t& residual_sum_times_four,
                                 uint64_t& residual_count);
bool banding_tile_avx2_u8(const LumaPlaneView& plane,
                          int tile_x,
                          int tile_y,
                          int tile_width,
                          int tile_height,
                          BandingTileStats& stats);

}  // namespace vr::analysis::quality::cpu
