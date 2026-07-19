#include "analysis/quality/quality_metrics.h"
#include "analysis/quality/quality_metrics_cpu.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace vr::analysis::quality {
namespace {

double clamp_unit(double value) {
    return std::max(0.0, std::min(1.0, value));
}

uint32_t max_sample_value(const LumaPlaneView& plane) {
    if (plane.bit_depth >= 31) {
        return std::numeric_limits<uint32_t>::max();
    }
    return (uint32_t{1} << plane.bit_depth) - 1;
}

uint32_t read_sample(const LumaPlaneView& plane, int x, int y) {
    const uint8_t* ptr =
        plane.data +
        static_cast<ptrdiff_t>(y) * plane.stride_bytes +
        static_cast<ptrdiff_t>(x) * plane.sample_step_bytes +
        plane.sample_offset_bytes;

    uint32_t value = 0;
    if (plane.sample_step_bytes == 1) {
        value = *ptr;
    } else {
        uint16_t stored = 0;
        std::memcpy(&stored, ptr, sizeof(stored));
        value = stored;
    }
    return value >> plane.sample_shift;
}

uint8_t read_sample_u8(const LumaPlaneView& plane, int x, int y) {
    const uint32_t maximum = max_sample_value(plane);
    const uint64_t value = read_sample(plane, x, y);
    return static_cast<uint8_t>((value * 255u + maximum / 2u) / maximum);
}

struct DifferenceTotals {
    double boundary_sum = 0.0;
    uint64_t boundary_count = 0;
    double interior_sum = 0.0;
    uint64_t interior_count = 0;
};

double normalized_excess(const DifferenceTotals& totals) {
    if (totals.boundary_count == 0 || totals.interior_count == 0) {
        return 0.0;
    }
    const double boundary =
        totals.boundary_sum / static_cast<double>(totals.boundary_count);
    const double interior =
        totals.interior_sum / static_cast<double>(totals.interior_count);
    const double denominator = boundary + interior;
    if (denominator <= 1e-12) {
        return 0.0;
    }
    return clamp_unit((boundary - interior) / denominator);
}

double blockiness_for_period(const LumaPlaneView& plane, int period) {
    DifferenceTotals horizontal;
    DifferenceTotals vertical;
    const double scale = 1.0 / static_cast<double>(max_sample_value(plane));

    for (int y = 0; y < plane.height; ++y) {
        for (int x = 1; x < plane.width; ++x) {
            const double difference =
                std::abs(static_cast<double>(read_sample(plane, x, y)) -
                         static_cast<double>(read_sample(plane, x - 1, y))) *
                scale;
            if (x % period == 0) {
                horizontal.boundary_sum += difference;
                ++horizontal.boundary_count;
            } else if (x % period != 1 && x % period != period - 1) {
                horizontal.interior_sum += difference;
                ++horizontal.interior_count;
            }
        }
    }

    for (int y = 1; y < plane.height; ++y) {
        for (int x = 0; x < plane.width; ++x) {
            const double difference =
                std::abs(static_cast<double>(read_sample(plane, x, y)) -
                         static_cast<double>(read_sample(plane, x, y - 1))) *
                scale;
            if (y % period == 0) {
                vertical.boundary_sum += difference;
                ++vertical.boundary_count;
            } else if (y % period != 1 && y % period != period - 1) {
                vertical.interior_sum += difference;
                ++vertical.interior_count;
            }
        }
    }

    return 0.5 * (normalized_excess(horizontal) +
                  normalized_excess(vertical));
}

double blockiness_for_period_avx2(const LumaPlaneView& plane,
                                  int period) {
    cpu::BlockinessPeriodSums sums;
    if (!cpu::blockiness_period_avx2_u8(plane, period, sums)) {
        return -1.0;
    }
    const double scale =
        1.0 / static_cast<double>(max_sample_value(plane));
    DifferenceTotals horizontal;
    horizontal.boundary_sum =
        static_cast<double>(sums.horizontal_boundary) * scale;
    horizontal.boundary_count = sums.horizontal_boundary_count;
    horizontal.interior_sum =
        static_cast<double>(sums.horizontal_interior) * scale;
    horizontal.interior_count = sums.horizontal_interior_count;
    DifferenceTotals vertical;
    vertical.boundary_sum =
        static_cast<double>(sums.vertical_boundary) * scale;
    vertical.boundary_count = sums.vertical_boundary_count;
    vertical.interior_sum =
        static_cast<double>(sums.vertical_interior) * scale;
    vertical.interior_count = sums.vertical_interior_count;
    return 0.5 * (normalized_excess(horizontal) +
                  normalized_excess(vertical));
}

cpu::BlurDirectionSums blur_for_direction_scalar(
    const LumaPlaneView& plane,
    bool horizontal) {
    cpu::BlurDirectionSums sums;
    const int major_limit = horizontal ? plane.width : plane.height;
    const int minor_limit = horizontal ? plane.height : plane.width;
    std::vector<int> values(static_cast<size_t>(major_limit));
    for (int minor = 0; minor < minor_limit; ++minor) {
        for (int major = 0; major < major_limit; ++major) {
            const int x = horizontal ? major : minor;
            const int y = horizontal ? minor : major;
            values[static_cast<size_t>(major)] =
                read_sample_u8(plane, x, y);
        }
        for (int major = 1; major < major_limit; ++major) {
            const int add_major =
                std::min(major_limit - 1, major + 2);
            const int remove_major = std::max(0, major - 3);
            const int original_difference = std::abs(
                values[static_cast<size_t>(major)] -
                values[static_cast<size_t>(major - 1)]);
            if (original_difference < 4) {
                continue;
            }
            const int blurred_difference = std::abs(
                values[static_cast<size_t>(add_major)] -
                values[static_cast<size_t>(remove_major)]);
            const int original_times_five =
                original_difference * 5;
            sums.original_times_five +=
                static_cast<uint64_t>(original_times_five);
            sums.lost_times_five += static_cast<uint64_t>(
                std::max(
                    0, original_times_five - blurred_difference));
            ++sums.edge_count;
        }
    }
    return sums;
}

double blur_score(const cpu::BlurDirectionSums& sums) {
    if (sums.edge_count == 0 ||
        sums.original_times_five == 0) {
        return -1.0;
    }
    return clamp_unit(
        1.0 -
        static_cast<double>(sums.lost_times_five) /
            static_cast<double>(sums.original_times_five));
}

double blur_for_direction(const LumaPlaneView& plane,
                          bool horizontal,
                          QualityCpuMode mode) {
    if (mode == QualityCpuMode::Auto && cpu::avx2_is_available()) {
        cpu::BlurDirectionSums sums;
        if (cpu::blur_direction_avx2_u8(
                plane, horizontal, sums)) {
            return blur_score(sums);
        }
    }
    return blur_score(
        blur_for_direction_scalar(plane, horizontal));
}

double median(std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    const size_t middle = values.size() / 2;
    std::nth_element(
        values.begin(), values.begin() + static_cast<ptrdiff_t>(middle),
        values.end());
    if (values.size() % 2 != 0) {
        return values[middle];
    }
    const double upper = values[middle];
    const double lower =
        *std::max_element(values.begin(),
                          values.begin() + static_cast<ptrdiff_t>(middle));
    return 0.5 * (lower + upper);
}

struct NoiseTile {
    double gradient = 0.0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

double signature_distance(const LumaTemporalSignature& left,
                          const LumaTemporalSignature& right) {
    if (left.tile_means.empty() ||
        left.tile_means.size() != right.tile_means.size()) {
        return std::numeric_limits<double>::infinity();
    }
    double sum = 0.0;
    for (size_t index = 0; index < left.tile_means.size(); ++index) {
        sum += std::abs(left.tile_means[index] -
                        right.tile_means[index]);
    }
    return sum /
           (static_cast<double>(left.tile_means.size()) * 255.0);
}

double quantile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double position =
        fraction * static_cast<double>(sorted.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = static_cast<size_t>(std::ceil(position));
    if (lower == upper) {
        return sorted[lower];
    }
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

}  // namespace

bool is_valid_luma_plane(const LumaPlaneView& plane) {
    if (!plane.data || plane.width < 2 || plane.height < 2 ||
        plane.stride_bytes == 0 || plane.bit_depth <= 0 ||
        plane.bit_depth > 16 || plane.sample_shift < 0 ||
        plane.sample_step_bytes <= 0 ||
        plane.sample_offset_bytes < 0 ||
        plane.sample_offset_bytes >= plane.sample_step_bytes) {
        return false;
    }
    if (plane.sample_step_bytes != 1 && plane.sample_step_bytes != 2) {
        return false;
    }
    return plane.bit_depth + plane.sample_shift <=
           plane.sample_step_bytes * 8;
}

double measure_blockiness(const LumaPlaneView& plane) {
    return measure_blockiness(plane, QualityCpuMode::Auto);
}

double measure_blockiness(const LumaPlaneView& plane,
                          QualityCpuMode mode) {
    if (!is_valid_luma_plane(plane) || plane.width < 24 ||
        plane.height < 24) {
        return 0.0;
    }
    if (mode == QualityCpuMode::Auto && cpu::avx2_is_available()) {
        const double at_eight =
            blockiness_for_period_avx2(plane, 8);
        const double at_sixteen =
            blockiness_for_period_avx2(plane, 16);
        if (at_eight >= 0.0 && at_sixteen >= 0.0) {
            return clamp_unit(
                0.65 * at_eight + 0.35 * at_sixteen);
        }
    }
    const double at_eight = blockiness_for_period(plane, 8);
    const double at_sixteen = blockiness_for_period(plane, 16);
    return clamp_unit(0.65 * at_eight + 0.35 * at_sixteen);
}

const char* quality_cpu_dispatch_name() {
    return cpu::avx2_is_available() ? "avx2" : "scalar";
}

double measure_banding_proxy(const LumaPlaneView& plane) {
    return measure_banding_proxy(plane, QualityCpuMode::Auto);
}

double measure_banding_proxy(const LumaPlaneView& plane,
                             QualityCpuMode mode) {
    if (!is_valid_luma_plane(plane) || plane.width < 8 ||
        plane.height < 8) {
        return 0.0;
    }

    constexpr int kTileSize = 16;
    const bool use_avx2 =
        mode == QualityCpuMode::Auto &&
        cpu::avx2_is_available();
    double weighted_score = 0.0;
    uint64_t weighted_pixels = 0;

    for (int tile_y = 0; tile_y < plane.height; tile_y += kTileSize) {
        const int tile_height =
            std::min(kTileSize, plane.height - tile_y);
        if (tile_height < 8) {
            continue;
        }
        for (int tile_x = 0; tile_x < plane.width; tile_x += kTileSize) {
            const int tile_width =
                std::min(kTileSize, plane.width - tile_x);
            if (tile_width < 8) {
                continue;
            }

            std::array<bool, 256> present{};
            int minimum = 255;
            int maximum = 0;
            uint64_t weak_contours = 0;
            uint64_t edge_count = 0;
            double gradient_sum = 0.0;
            cpu::BandingTileStats avx2_stats;
            const bool used_avx2 =
                use_avx2 &&
                cpu::banding_tile_avx2_u8(
                    plane,
                    tile_x,
                    tile_y,
                    tile_width,
                    tile_height,
                    avx2_stats);
            int unique_values = 0;
            if (used_avx2) {
                minimum = avx2_stats.minimum;
                maximum = avx2_stats.maximum;
                weak_contours = avx2_stats.weak_contours;
                edge_count = avx2_stats.edge_count;
                gradient_sum =
                    static_cast<double>(avx2_stats.gradient_sum);
                for (uint64_t bits : avx2_stats.present) {
                    while (bits != 0) {
                        bits &= bits - 1;
                        ++unique_values;
                    }
                }
            } else {
                for (int y = 0; y < tile_height; ++y) {
                    for (int x = 0; x < tile_width; ++x) {
                        const int value = read_sample_u8(
                            plane, tile_x + x, tile_y + y);
                        present[static_cast<size_t>(value)] = true;
                        minimum = std::min(minimum, value);
                        maximum = std::max(maximum, value);

                        if (x + 1 < tile_width) {
                            const int next = read_sample_u8(
                                plane,
                                tile_x + x + 1,
                                tile_y + y);
                            const int difference =
                                std::abs(value - next);
                            gradient_sum += difference;
                            ++edge_count;
                            if (difference >= 1 &&
                                difference <= 8) {
                                ++weak_contours;
                            }
                        }
                        if (y + 1 < tile_height) {
                            const int next = read_sample_u8(
                                plane,
                                tile_x + x,
                                tile_y + y + 1);
                            const int difference =
                                std::abs(value - next);
                            gradient_sum += difference;
                            ++edge_count;
                            if (difference >= 1 &&
                                difference <= 8) {
                                ++weak_contours;
                            }
                        }
                    }
                }

                for (const bool exists : present) {
                    unique_values += exists ? 1 : 0;
                }
            }
            const int dynamic_range = maximum - minimum;
            if (unique_values < 2 || dynamic_range < 2 ||
                dynamic_range > 32 || edge_count == 0) {
                continue;
            }

            const double mean_gradient =
                gradient_sum / static_cast<double>(edge_count);
            if (mean_gradient > 4.0) {
                continue;
            }

            const double quantization =
                clamp_unit((12.0 - unique_values) / 10.0);
            const double range_weight =
                clamp_unit(dynamic_range / 16.0);
            const double contour_density =
                static_cast<double>(weak_contours) /
                static_cast<double>(edge_count);
            const double contour_weight =
                clamp_unit(contour_density * 8.0);
            const double tile_score =
                quantization * range_weight * contour_weight;
            const uint64_t pixels =
                static_cast<uint64_t>(tile_width) * tile_height;
            weighted_score += tile_score * static_cast<double>(pixels);
            weighted_pixels += pixels;
        }
    }

    if (weighted_pixels == 0) {
        return 0.0;
    }
    return clamp_unit(weighted_score /
                      static_cast<double>(weighted_pixels));
}

double measure_blur_proxy(const LumaPlaneView& plane) {
    return measure_blur_proxy(plane, QualityCpuMode::Auto);
}

double measure_blur_proxy(const LumaPlaneView& plane,
                          QualityCpuMode mode) {
    if (!is_valid_luma_plane(plane) || plane.width < 8 ||
        plane.height < 8) {
        return 0.0;
    }
    const double horizontal =
        blur_for_direction(plane, true, mode);
    const double vertical =
        blur_for_direction(plane, false, mode);
    if (horizontal < 0.0 && vertical < 0.0) {
        return 0.0;
    }
    if (horizontal < 0.0) {
        return vertical;
    }
    if (vertical < 0.0) {
        return horizontal;
    }
    return std::max(horizontal, vertical);
}

double measure_noise_proxy(const LumaPlaneView& plane) {
    return measure_noise_proxy(plane, QualityCpuMode::Auto);
}

double measure_noise_proxy(const LumaPlaneView& plane,
                           QualityCpuMode mode) {
    if (!is_valid_luma_plane(plane) || plane.width < 8 ||
        plane.height < 8) {
        return 0.0;
    }

    constexpr int kTileSize = 16;
    const bool use_avx2 =
        mode == QualityCpuMode::Auto &&
        cpu::avx2_is_available();
    std::vector<NoiseTile> tiles;
    for (int tile_y = 0; tile_y < plane.height; tile_y += kTileSize) {
        const int tile_height =
            std::min(kTileSize, plane.height - tile_y);
        if (tile_height < 8) {
            continue;
        }
        for (int tile_x = 0; tile_x < plane.width; tile_x += kTileSize) {
            const int tile_width =
                std::min(kTileSize, plane.width - tile_x);
            if (tile_width < 8) {
                continue;
            }
            double gradient_sum = 0.0;
            uint64_t gradient_count = 0;
            uint64_t gradient_sum_integer = 0;
            const bool used_avx2 =
                use_avx2 &&
                cpu::noise_tile_gradient_avx2_u8(
                    plane,
                    tile_x,
                    tile_y,
                    tile_width,
                    tile_height,
                    gradient_sum_integer,
                    gradient_count);
            if (used_avx2) {
                gradient_sum =
                    static_cast<double>(gradient_sum_integer);
            } else {
                for (int y = 1; y + 1 < tile_height; ++y) {
                    for (int x = 1; x + 1 < tile_width; ++x) {
                        const int center =
                            read_sample_u8(
                                plane, tile_x + x, tile_y + y);
                        gradient_sum += std::abs(
                            center -
                            static_cast<int>(read_sample_u8(
                                plane,
                                tile_x + x + 1,
                                tile_y + y)));
                        gradient_sum += std::abs(
                            center -
                            static_cast<int>(read_sample_u8(
                                plane,
                                tile_x + x,
                                tile_y + y + 1)));
                        gradient_count += 2;
                    }
                }
            }
            if (gradient_count > 0) {
                tiles.push_back(NoiseTile{
                    gradient_sum /
                        static_cast<double>(gradient_count),
                    tile_x,
                    tile_y,
                    tile_width,
                    tile_height,
                });
            }
        }
    }
    if (tiles.empty()) {
        return 0.0;
    }

    std::sort(tiles.begin(), tiles.end(),
              [](const NoiseTile& left, const NoiseTile& right) {
                  if (left.gradient != right.gradient) {
                      return left.gradient < right.gradient;
                  }
                  if (left.y != right.y) {
                      return left.y < right.y;
                  }
                  return left.x < right.x;
              });
    const size_t selected_count =
        std::max<size_t>(1, (tiles.size() + 3) / 4);
    double residual_sum = 0.0;
    uint64_t residual_count = 0;
    for (size_t index = 0; index < selected_count; ++index) {
        const NoiseTile& tile = tiles[index];
        uint64_t residual_sum_times_four = 0;
        uint64_t tile_residual_count = 0;
        const bool used_avx2 =
            use_avx2 &&
            cpu::noise_tile_residual_avx2_u8(
                plane,
                tile.x,
                tile.y,
                tile.width,
                tile.height,
                residual_sum_times_four,
                tile_residual_count);
        if (used_avx2) {
            residual_sum +=
                static_cast<double>(residual_sum_times_four) /
                4.0;
            residual_count += tile_residual_count;
        } else {
            for (int y = 1; y + 1 < tile.height; ++y) {
                for (int x = 1; x + 1 < tile.width; ++x) {
                    const int center =
                        read_sample_u8(
                            plane, tile.x + x, tile.y + y);
                    const int laplacian =
                        4 * center -
                        read_sample_u8(
                            plane,
                            tile.x + x - 1,
                            tile.y + y) -
                        read_sample_u8(
                            plane,
                            tile.x + x + 1,
                            tile.y + y) -
                        read_sample_u8(
                            plane,
                            tile.x + x,
                            tile.y + y - 1) -
                        read_sample_u8(
                            plane,
                            tile.x + x,
                            tile.y + y + 1);
                    residual_sum += std::min(
                        std::abs(
                            static_cast<double>(laplacian)) /
                            4.0,
                        64.0);
                    ++residual_count;
                }
            }
        }
    }
    if (residual_count == 0) {
        return 0.0;
    }

    // Cap extreme residuals so a few edges do not dominate the estimate. For
    // the normalized 4-neighbour Laplacian, Gaussian noise has a
    // standard-deviation gain of sqrt(20)/4. Convert mean absolute response to
    // an approximate 8-bit sigma, then map sigma >= 24 to the top of the
    // experimental scale.
    const double mean_absolute =
        residual_sum / static_cast<double>(residual_count);
    constexpr double kGaussianMeanAbsolute = 0.7978845608028654;
    constexpr double kLaplacianGain = 1.118033988749895;
    const double sigma =
        mean_absolute / (kGaussianMeanAbsolute * kLaplacianGain);
    return clamp_unit(sigma / 24.0);
}

bool make_temporal_signature(const LumaPlaneView& plane,
                             LumaTemporalSignature& signature) {
    signature = LumaTemporalSignature{};
    if (!is_valid_luma_plane(plane)) {
        return false;
    }

    const int columns = std::min(16, plane.width);
    const int rows = std::min(9, plane.height);
    signature.tile_means.reserve(
        static_cast<size_t>(columns * rows));
    double frame_sum = 0.0;
    uint64_t frame_count = 0;
    for (int row = 0; row < rows; ++row) {
        const int y0 = row * plane.height / rows;
        const int y1 = (row + 1) * plane.height / rows;
        for (int column = 0; column < columns; ++column) {
            const int x0 = column * plane.width / columns;
            const int x1 = (column + 1) * plane.width / columns;
            double tile_sum = 0.0;
            uint64_t tile_count = 0;
            const int sample_rows = std::min(8, y1 - y0);
            const int sample_columns = std::min(8, x1 - x0);
            for (int sample_y = 0; sample_y < sample_rows; ++sample_y) {
                const int y = y0 +
                    (2 * sample_y + 1) * (y1 - y0) /
                        (2 * sample_rows);
                for (int sample_x = 0;
                     sample_x < sample_columns;
                     ++sample_x) {
                    const int x = x0 +
                        (2 * sample_x + 1) * (x1 - x0) /
                            (2 * sample_columns);
                    tile_sum += read_sample_u8(plane, x, y);
                    ++tile_count;
                }
            }
            if (tile_count == 0) {
                signature = LumaTemporalSignature{};
                return false;
            }
            signature.tile_means.push_back(
                tile_sum / static_cast<double>(tile_count));
            frame_sum += tile_sum;
            frame_count += tile_count;
        }
    }
    signature.mean_luma =
        frame_sum / static_cast<double>(frame_count);
    return true;
}

double measure_flicker_proxy(
    const LumaTemporalSignature& previous_previous,
    const LumaTemporalSignature& previous,
    const LumaTemporalSignature& current) {
    if (previous_previous.tile_means.empty() ||
        previous_previous.tile_means.size() != previous.tile_means.size() ||
        previous.tile_means.size() != current.tile_means.size()) {
        return -1.0;
    }

    constexpr double kSceneCutDistance = 0.20;
    if (signature_distance(previous_previous, previous) >
            kSceneCutDistance ||
        signature_distance(previous, current) > kSceneCutDistance) {
        return -1.0;
    }

    const double global_curvature =
        std::abs(current.mean_luma - 2.0 * previous.mean_luma +
                 previous_previous.mean_luma) /
        255.0;
    std::vector<double> local_curvature;
    local_curvature.reserve(current.tile_means.size());
    for (size_t index = 0; index < current.tile_means.size(); ++index) {
        local_curvature.push_back(
            std::abs(current.tile_means[index] -
                     2.0 * previous.tile_means[index] +
                     previous_previous.tile_means[index]) /
            255.0);
    }
    const double local_median = median(local_curvature);
    return clamp_unit(
        4.0 * (0.75 * global_curvature + 0.25 * local_median));
}

DistributionSummary summarize_distribution(std::vector<double> values) {
    DistributionSummary summary;
    values.erase(
        std::remove_if(values.begin(), values.end(), [](double value) {
            return !std::isfinite(value);
        }),
        values.end());
    if (values.empty()) {
        return summary;
    }

    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (const double value : values) {
        sum += value;
    }
    summary.count = values.size();
    summary.mean = sum / static_cast<double>(values.size());
    summary.p10 = quantile(values, 0.10);
    summary.p50 = quantile(values, 0.50);
    summary.p90 = quantile(values, 0.90);
    summary.p95 = quantile(values, 0.95);
    summary.maximum = values.back();
    return summary;
}

}  // namespace vr::analysis::quality
