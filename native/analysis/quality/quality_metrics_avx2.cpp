#include "analysis/quality/quality_metrics_cpu.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <limits>
#include <vector>

namespace vr::analysis::quality::cpu {
namespace {

struct HorizontalMasks {
    alignas(32) std::array<uint8_t, 32> boundary{};
    alignas(32) std::array<uint8_t, 32> interior{};
    uint64_t boundary_count = 0;
    uint64_t interior_count = 0;
};

struct HorizontalMasks16 {
    alignas(32) std::array<uint16_t, 16> boundary{};
    alignas(32) std::array<uint16_t, 16> interior{};
    uint64_t boundary_count = 0;
    uint64_t interior_count = 0;
};

HorizontalMasks make_horizontal_masks(int period) {
    HorizontalMasks masks;
    for (int lane = 0; lane < 32; ++lane) {
        const int x = lane + 1;
        if (x % period == 0) {
            masks.boundary[static_cast<size_t>(lane)] = 0xff;
            ++masks.boundary_count;
        } else if (x % period != 1 &&
                   x % period != period - 1) {
            masks.interior[static_cast<size_t>(lane)] = 0xff;
            ++masks.interior_count;
        }
    }
    return masks;
}

HorizontalMasks16 make_horizontal_masks16(int period) {
    HorizontalMasks16 masks;
    for (int lane = 0; lane < 16; ++lane) {
        const int x = lane + 1;
        if (x % period == 0) {
            masks.boundary[static_cast<size_t>(lane)] = 0xffff;
            ++masks.boundary_count;
        } else if (x % period != 1 &&
                   x % period != period - 1) {
            masks.interior[static_cast<size_t>(lane)] = 0xffff;
            ++masks.interior_count;
        }
    }
    return masks;
}

uint64_t horizontal_sum(__m256i differences,
                        const std::array<uint8_t, 32>& mask) {
    const __m256i selected = _mm256_and_si256(
        differences,
        _mm256_load_si256(
            reinterpret_cast<const __m256i*>(mask.data())));
    const __m256i sums =
        _mm256_sad_epu8(selected, _mm256_setzero_si256());
    alignas(32) uint64_t lanes[4] = {};
    _mm256_store_si256(
        reinterpret_cast<__m256i*>(lanes), sums);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}

uint64_t full_sum(__m256i differences) {
    const __m256i sums =
        _mm256_sad_epu8(differences, _mm256_setzero_si256());
    alignas(32) uint64_t lanes[4] = {};
    _mm256_store_si256(
        reinterpret_cast<__m256i*>(lanes), sums);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}

__m256i absolute_difference(__m256i left, __m256i right) {
    return _mm256_or_si256(
        _mm256_subs_epu8(left, right),
        _mm256_subs_epu8(right, left));
}

uint64_t sum_u16(__m256i values) {
    const __m256i pairs =
        _mm256_madd_epi16(values, _mm256_set1_epi16(1));
    alignas(32) uint32_t lanes[8] = {};
    _mm256_store_si256(
        reinterpret_cast<__m256i*>(lanes), pairs);
    uint64_t sum = 0;
    for (const uint32_t lane : lanes) {
        sum += lane;
    }
    return sum;
}

uint64_t sum_u16_unsigned(__m256i values) {
    const __m128i low = _mm256_castsi256_si128(values);
    const __m128i high = _mm256_extracti128_si256(values, 1);
    const __m256i low32 = _mm256_cvtepu16_epi32(low);
    const __m256i high32 = _mm256_cvtepu16_epi32(high);
    alignas(32) uint32_t lanes[8] = {};
    _mm256_store_si256(
        reinterpret_cast<__m256i*>(lanes), low32);
    uint64_t sum = 0;
    for (const uint32_t lane : lanes) {
        sum += lane;
    }
    _mm256_store_si256(
        reinterpret_cast<__m256i*>(lanes), high32);
    for (const uint32_t lane : lanes) {
        sum += lane;
    }
    return sum;
}

uint64_t count_mask_bytes(__m256i mask) {
    uint32_t bits = static_cast<uint32_t>(
        _mm256_movemask_epi8(mask));
    uint64_t count = 0;
    while (bits != 0) {
        bits &= bits - 1;
        ++count;
    }
    return count;
}

void accumulate_blur_vectors(__m256i original,
                             __m256i blurred,
                             BlurDirectionSums& sums) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i above_three =
        _mm256_subs_epu8(original, _mm256_set1_epi8(3));
    const __m256i edge_mask = _mm256_xor_si256(
        _mm256_cmpeq_epi8(above_three, zero),
        _mm256_set1_epi8(static_cast<char>(0xff)));
    const __m256i selected_original =
        _mm256_and_si256(original, edge_mask);
    sums.original_times_five +=
        full_sum(selected_original) * 5;
    sums.edge_count += count_mask_bytes(edge_mask);

    const __m128i original_low =
        _mm256_castsi256_si128(original);
    const __m128i original_high =
        _mm256_extracti128_si256(original, 1);
    const __m128i blurred_low =
        _mm256_castsi256_si128(blurred);
    const __m128i blurred_high =
        _mm256_extracti128_si256(blurred, 1);
    const __m128i mask_low =
        _mm256_castsi256_si128(edge_mask);
    const __m128i mask_high =
        _mm256_extracti128_si256(edge_mask, 1);

    const __m256i original16_low =
        _mm256_cvtepu8_epi16(original_low);
    const __m256i original16_high =
        _mm256_cvtepu8_epi16(original_high);
    const __m256i blurred16_low =
        _mm256_cvtepu8_epi16(blurred_low);
    const __m256i blurred16_high =
        _mm256_cvtepu8_epi16(blurred_high);
    const __m256i mask16_low =
        _mm256_cvtepi8_epi16(mask_low);
    const __m256i mask16_high =
        _mm256_cvtepi8_epi16(mask_high);
    const __m256i five = _mm256_set1_epi16(5);
    const __m256i loss_low = _mm256_and_si256(
        _mm256_subs_epu16(
            _mm256_mullo_epi16(original16_low, five),
            blurred16_low),
        mask16_low);
    const __m256i loss_high = _mm256_and_si256(
        _mm256_subs_epu16(
            _mm256_mullo_epi16(original16_high, five),
            blurred16_high),
        mask16_high);
    sums.lost_times_five +=
        sum_u16(loss_low) + sum_u16(loss_high);
}

void accumulate_blur_scalar(uint8_t current,
                            uint8_t previous,
                            uint8_t added,
                            uint8_t removed,
                            BlurDirectionSums& sums) {
    const int original =
        std::abs(static_cast<int>(current) -
                 static_cast<int>(previous));
    if (original < 4) {
        return;
    }
    const int blurred =
        std::abs(static_cast<int>(added) -
                 static_cast<int>(removed));
    const int original_times_five = original * 5;
    sums.original_times_five +=
        static_cast<uint64_t>(original_times_five);
    sums.lost_times_five += static_cast<uint64_t>(
        std::max(0, original_times_five - blurred));
    ++sums.edge_count;
}

bool is_packed_u8(const LumaPlaneView& plane) {
    return plane.sample_step_bytes == 1 &&
           plane.sample_offset_bytes == 0 &&
           plane.bit_depth == 8 &&
           plane.sample_shift == 0;
}

bool is_packed_u16(const LumaPlaneView& plane) {
    return plane.sample_step_bytes == 2 &&
           plane.sample_offset_bytes == 0 &&
           plane.bit_depth >= 9 &&
           plane.bit_depth <= 16 &&
           plane.sample_shift >= 0 &&
           plane.bit_depth + plane.sample_shift <= 16;
}

uint16_t read_raw_u16(const uint8_t* row,
                      int x,
                      int sample_shift) {
    uint16_t value = 0;
    std::memcpy(&value, row + static_cast<ptrdiff_t>(x) * 2,
                sizeof(value));
    return static_cast<uint16_t>(value >> sample_shift);
}

const int* normalized_u8_lut(int bit_depth) {
    struct Lut {
        int bit_depth = 0;
        std::vector<int> values;
    };
    thread_local Lut lut;
    if (lut.bit_depth != bit_depth) {
        const uint32_t maximum =
            (uint32_t{1} << bit_depth) - 1;
        lut.values.resize(static_cast<size_t>(maximum) + 1);
        for (uint32_t value = 0; value <= maximum; ++value) {
            lut.values[value] = static_cast<int>(
                (static_cast<uint64_t>(value) * 255u +
                 maximum / 2u) /
                maximum);
        }
        lut.bit_depth = bit_depth;
    }
    return lut.values.data();
}

__m128i load_normalized_u16(const uint8_t* address,
                            int bit_depth,
                            int sample_shift) {
    __m256i stored = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(address));
    if (sample_shift != 0) {
        stored = _mm256_srl_epi16(
            stored, _mm_cvtsi32_si128(sample_shift));
    }
    const __m256i low32 = _mm256_cvtepu16_epi32(
        _mm256_castsi256_si128(stored));
    const __m256i high32 = _mm256_cvtepu16_epi32(
        _mm256_extracti128_si256(stored, 1));
    const int* lut = normalized_u8_lut(bit_depth);
    const __m256i normalized_low =
        _mm256_i32gather_epi32(lut, low32, 4);
    const __m256i normalized_high =
        _mm256_i32gather_epi32(lut, high32, 4);
    const __m256i packed16 = _mm256_permute4x64_epi64(
        _mm256_packus_epi32(normalized_low, normalized_high),
        0xd8);
    return _mm_packus_epi16(
        _mm256_castsi256_si128(packed16),
        _mm256_extracti128_si256(packed16, 1));
}

__m256i load_normalized_32(const uint8_t* address,
                           int bit_depth,
                           int sample_shift) {
    const __m128i low =
        load_normalized_u16(address, bit_depth, sample_shift);
    const __m128i high =
        load_normalized_u16(address + 32, bit_depth, sample_shift);
    return _mm256_inserti128_si256(
        _mm256_castsi128_si256(low), high, 1);
}

uint8_t normalize_u16_scalar(uint16_t value, int bit_depth) {
    return static_cast<uint8_t>(
        normalized_u8_lut(bit_depth)[value]);
}

uint8_t read_normalized_u16(const uint8_t* row,
                            int x,
                            int bit_depth,
                            int sample_shift) {
    return normalize_u16_scalar(
        read_raw_u16(row, x, sample_shift), bit_depth);
}

const std::array<uint8_t, 16>& noise_byte_mask() {
    static const std::array<uint8_t, 16> mask = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00,
    };
    return mask;
}

const std::array<uint8_t, 16>& banding_horizontal_mask() {
    static const std::array<uint8_t, 16> mask = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
    };
    return mask;
}

uint64_t sum_16_bytes(__m128i values) {
    const __m128i sums =
        _mm_sad_epu8(values, _mm_setzero_si128());
    alignas(16) uint64_t lanes[2] = {};
    _mm_store_si128(
        reinterpret_cast<__m128i*>(lanes), sums);
    return lanes[0] + lanes[1];
}

uint64_t sum_noise_bytes(__m128i values) {
    const auto& mask = noise_byte_mask();
    const __m128i selected = _mm_and_si128(
        values,
        _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(mask.data())));
    const __m128i sums =
        _mm_sad_epu8(selected, _mm_setzero_si128());
    alignas(16) uint64_t lanes[2] = {};
    _mm_store_si128(
        reinterpret_cast<__m128i*>(lanes), sums);
    return lanes[0] + lanes[1];
}

uint64_t count_16_mask_bytes(__m128i mask) {
    uint32_t bits = static_cast<uint32_t>(
        _mm_movemask_epi8(mask));
    uint64_t count = 0;
    while (bits != 0) {
        bits &= bits - 1;
        ++count;
    }
    return count;
}

uint64_t count_weak_differences(__m128i differences,
                                __m128i valid_mask) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i nonzero = _mm_xor_si128(
        _mm_cmpeq_epi8(differences, zero),
        _mm_set1_epi8(static_cast<char>(0xff)));
    const __m128i at_most_eight = _mm_cmpeq_epi8(
        _mm_subs_epu8(differences, _mm_set1_epi8(8)),
        zero);
    return count_16_mask_bytes(
        _mm_and_si128(
            _mm_and_si128(nonzero, at_most_eight),
            valid_mask));
}

__m256i noise_word_mask() {
    return _mm256_setr_epi16(
        -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1,
        0, 0);
}

}  // namespace

bool blockiness_period_avx2_u8(const LumaPlaneView& plane,
                              int period,
                              BlockinessPeriodSums& sums) {
    sums = BlockinessPeriodSums{};
    if ((period != 8 && period != 16) ||
        (!is_packed_u8(plane) && !is_packed_u16(plane))) {
        return false;
    }

    if (is_packed_u16(plane)) {
        const HorizontalMasks16 masks =
            make_horizontal_masks16(period);
        int vector_end = 1 + ((plane.width - 1) / 16) * 16;
        for (int y = 0; y < plane.height; ++y) {
            const uint8_t* row =
                plane.data +
                static_cast<ptrdiff_t>(y) * plane.stride_bytes;
            int x = 1;
            for (; x < vector_end; x += 16) {
                __m256i current = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(
                        row + static_cast<ptrdiff_t>(x) * 2));
                __m256i previous = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(
                        row + static_cast<ptrdiff_t>(x - 1) * 2));
                if (plane.sample_shift != 0) {
                    const __m128i shift_count =
                        _mm_cvtsi32_si128(plane.sample_shift);
                    current = _mm256_srl_epi16(
                        current, shift_count);
                    previous = _mm256_srl_epi16(
                        previous, shift_count);
                }
                const __m256i differences = _mm256_sub_epi16(
                    _mm256_max_epu16(current, previous),
                    _mm256_min_epu16(current, previous));
                sums.horizontal_boundary += sum_u16_unsigned(
                    _mm256_and_si256(
                        differences,
                        _mm256_load_si256(
                            reinterpret_cast<const __m256i*>(
                                masks.boundary.data()))));
                sums.horizontal_interior += sum_u16_unsigned(
                    _mm256_and_si256(
                        differences,
                        _mm256_load_si256(
                            reinterpret_cast<const __m256i*>(
                                masks.interior.data()))));
                sums.horizontal_boundary_count +=
                    masks.boundary_count;
                sums.horizontal_interior_count +=
                    masks.interior_count;
            }
            for (; x < plane.width; ++x) {
                const uint64_t difference = static_cast<uint64_t>(
                    std::abs(
                        static_cast<int>(read_raw_u16(
                            row, x, plane.sample_shift)) -
                        static_cast<int>(read_raw_u16(
                            row, x - 1, plane.sample_shift))));
                if (x % period == 0) {
                    sums.horizontal_boundary += difference;
                    ++sums.horizontal_boundary_count;
                } else if (x % period != 1 &&
                           x % period != period - 1) {
                    sums.horizontal_interior += difference;
                    ++sums.horizontal_interior_count;
                }
            }
        }

        vector_end = (plane.width / 16) * 16;
        for (int y = 1; y < plane.height; ++y) {
            const uint8_t* row =
                plane.data +
                static_cast<ptrdiff_t>(y) * plane.stride_bytes;
            const uint8_t* previous_row =
                row - plane.stride_bytes;
            uint64_t row_sum = 0;
            int x = 0;
            for (; x < vector_end; x += 16) {
                __m256i current = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(
                        row + static_cast<ptrdiff_t>(x) * 2));
                __m256i previous = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(
                        previous_row +
                        static_cast<ptrdiff_t>(x) * 2));
                if (plane.sample_shift != 0) {
                    const __m128i shift_count =
                        _mm_cvtsi32_si128(plane.sample_shift);
                    current = _mm256_srl_epi16(
                        current, shift_count);
                    previous = _mm256_srl_epi16(
                        previous, shift_count);
                }
                row_sum += sum_u16_unsigned(
                    _mm256_sub_epi16(
                        _mm256_max_epu16(current, previous),
                        _mm256_min_epu16(current, previous)));
            }
            for (; x < plane.width; ++x) {
                row_sum += static_cast<uint64_t>(
                    std::abs(
                        static_cast<int>(read_raw_u16(
                            row, x, plane.sample_shift)) -
                        static_cast<int>(read_raw_u16(
                            previous_row,
                            x,
                            plane.sample_shift))));
            }
            if (y % period == 0) {
                sums.vertical_boundary += row_sum;
                sums.vertical_boundary_count +=
                    static_cast<uint64_t>(plane.width);
            } else if (y % period != 1 &&
                       y % period != period - 1) {
                sums.vertical_interior += row_sum;
                sums.vertical_interior_count +=
                    static_cast<uint64_t>(plane.width);
            }
        }
        _mm256_zeroupper();
        return true;
    }

    const HorizontalMasks masks = make_horizontal_masks(period);
    int vector_end = 1 + ((plane.width - 1) / 32) * 32;
    for (int y = 0; y < plane.height; ++y) {
        const uint8_t* row =
            plane.data + static_cast<ptrdiff_t>(y) * plane.stride_bytes;
        int x = 1;
        for (; x < vector_end; x += 32) {
            const __m256i current = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(row + x));
            const __m256i previous = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(row + x - 1));
            const __m256i differences =
                absolute_difference(current, previous);
            sums.horizontal_boundary +=
                horizontal_sum(differences, masks.boundary);
            sums.horizontal_interior +=
                horizontal_sum(differences, masks.interior);
            sums.horizontal_boundary_count += masks.boundary_count;
            sums.horizontal_interior_count += masks.interior_count;
        }
        for (; x < plane.width; ++x) {
            const uint64_t difference = static_cast<uint64_t>(
                std::abs(static_cast<int>(row[x]) -
                         static_cast<int>(row[x - 1])));
            if (x % period == 0) {
                sums.horizontal_boundary += difference;
                ++sums.horizontal_boundary_count;
            } else if (x % period != 1 &&
                       x % period != period - 1) {
                sums.horizontal_interior += difference;
                ++sums.horizontal_interior_count;
            }
        }
    }

    vector_end = (plane.width / 32) * 32;
    for (int y = 1; y < plane.height; ++y) {
        const uint8_t* row =
            plane.data + static_cast<ptrdiff_t>(y) * plane.stride_bytes;
        const uint8_t* previous_row = row - plane.stride_bytes;
        uint64_t row_sum = 0;
        int x = 0;
        for (; x < vector_end; x += 32) {
            const __m256i current = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(row + x));
            const __m256i previous = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(previous_row + x));
            row_sum += full_sum(
                absolute_difference(current, previous));
        }
        for (; x < plane.width; ++x) {
            row_sum += static_cast<uint64_t>(
                std::abs(static_cast<int>(row[x]) -
                         static_cast<int>(previous_row[x])));
        }
        if (y % period == 0) {
            sums.vertical_boundary += row_sum;
            sums.vertical_boundary_count +=
                static_cast<uint64_t>(plane.width);
        } else if (y % period != 1 &&
                   y % period != period - 1) {
            sums.vertical_interior += row_sum;
            sums.vertical_interior_count +=
                static_cast<uint64_t>(plane.width);
        }
    }
    _mm256_zeroupper();
    return true;
}

bool blur_direction_avx2_u8(const LumaPlaneView& plane,
                            bool horizontal,
                            BlurDirectionSums& sums) {
    sums = BlurDirectionSums{};
    const bool packed_u16 = is_packed_u16(plane);
    if (!is_packed_u8(plane) && !packed_u16) {
        return false;
    }
    const auto load32 = [&](const uint8_t* row, int x) {
        if (packed_u16) {
            return load_normalized_32(
                row + static_cast<ptrdiff_t>(x) * 2,
                plane.bit_depth,
                plane.sample_shift);
        }
        return _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(row + x));
    };
    const auto read8 = [&](const uint8_t* row, int x) {
        return packed_u16
                   ? read_normalized_u16(
                         row,
                         x,
                         plane.bit_depth,
                         plane.sample_shift)
                   : row[x];
    };

    if (horizontal) {
        for (int y = 0; y < plane.height; ++y) {
            const uint8_t* row =
                plane.data +
                static_cast<ptrdiff_t>(y) * plane.stride_bytes;
            const int prefix_end = std::min(3, plane.width);
            int x = 1;
            for (; x < prefix_end; ++x) {
                accumulate_blur_scalar(
                    read8(row, x),
                    read8(row, x - 1),
                    read8(row, std::min(plane.width - 1, x + 2)),
                    read8(row, std::max(0, x - 3)),
                    sums);
            }
            const int vector_end =
                3 + std::max(0, (plane.width - 5) / 32) * 32;
            for (; x < vector_end; x += 32) {
                const __m256i current = load32(row, x);
                const __m256i previous = load32(row, x - 1);
                const __m256i added = load32(row, x + 2);
                const __m256i removed = load32(row, x - 3);
                accumulate_blur_vectors(
                    absolute_difference(current, previous),
                    absolute_difference(added, removed),
                    sums);
            }
            for (; x < plane.width; ++x) {
                accumulate_blur_scalar(
                    read8(row, x),
                    read8(row, x - 1),
                    read8(row, std::min(plane.width - 1, x + 2)),
                    read8(row, std::max(0, x - 3)),
                    sums);
            }
        }
    } else {
        const int vector_end = (plane.width / 32) * 32;
        for (int y = 1; y < plane.height; ++y) {
            const uint8_t* current =
                plane.data +
                static_cast<ptrdiff_t>(y) * plane.stride_bytes;
            const uint8_t* previous =
                current - plane.stride_bytes;
            const uint8_t* added =
                plane.data +
                static_cast<ptrdiff_t>(
                    std::min(plane.height - 1, y + 2)) *
                    plane.stride_bytes;
            const uint8_t* removed =
                plane.data +
                static_cast<ptrdiff_t>(std::max(0, y - 3)) *
                    plane.stride_bytes;
            int x = 0;
            for (; x < vector_end; x += 32) {
                const __m256i current_values = load32(current, x);
                const __m256i previous_values = load32(previous, x);
                const __m256i added_values = load32(added, x);
                const __m256i removed_values = load32(removed, x);
                accumulate_blur_vectors(
                    absolute_difference(
                        current_values, previous_values),
                    absolute_difference(
                        added_values, removed_values),
                    sums);
            }
            for (; x < plane.width; ++x) {
                accumulate_blur_scalar(
                    read8(current, x),
                    read8(previous, x),
                    read8(added, x),
                    read8(removed, x),
                    sums);
            }
        }
    }
    _mm256_zeroupper();
    return true;
}

bool noise_tile_gradient_avx2_u8(const LumaPlaneView& plane,
                                 int tile_x,
                                 int tile_y,
                                 int tile_width,
                                 int tile_height,
                                 uint64_t& gradient_sum,
                                 uint64_t& gradient_count) {
    gradient_sum = 0;
    gradient_count = 0;
    const bool packed_u16 = is_packed_u16(plane);
    if ((!is_packed_u8(plane) && !packed_u16) ||
        tile_width != 16 || tile_height != 16) {
        return false;
    }
    for (int y = 1; y < 15; ++y) {
        const uint8_t* row =
            plane.data +
            static_cast<ptrdiff_t>(tile_y + y) *
                plane.stride_bytes +
            static_cast<ptrdiff_t>(tile_x) *
                plane.sample_step_bytes;
        const uint8_t* down = row + plane.stride_bytes;
        const __m128i row_values =
            packed_u16
                ? load_normalized_u16(
                      row, plane.bit_depth, plane.sample_shift)
                : _mm_loadu_si128(
                      reinterpret_cast<const __m128i*>(row));
        const __m128i down_values =
            packed_u16
                ? load_normalized_u16(
                      down, plane.bit_depth, plane.sample_shift)
                : _mm_loadu_si128(
                      reinterpret_cast<const __m128i*>(down));
        const __m128i center =
            _mm_srli_si128(row_values, 1);
        const __m128i right =
            _mm_srli_si128(row_values, 2);
        const __m128i down_center =
            _mm_srli_si128(down_values, 1);
        gradient_sum += sum_noise_bytes(
            _mm_or_si128(
                _mm_subs_epu8(center, right),
                _mm_subs_epu8(right, center)));
        gradient_sum += sum_noise_bytes(
            _mm_or_si128(
                _mm_subs_epu8(center, down_center),
                _mm_subs_epu8(down_center, center)));
        gradient_count += 28;
    }
    _mm256_zeroupper();
    return true;
}

bool noise_tile_residual_avx2_u8(
    const LumaPlaneView& plane,
    int tile_x,
    int tile_y,
    int tile_width,
    int tile_height,
    uint64_t& residual_sum_times_four,
    uint64_t& residual_count) {
    residual_sum_times_four = 0;
    residual_count = 0;
    const bool packed_u16 = is_packed_u16(plane);
    if ((!is_packed_u8(plane) && !packed_u16) ||
        tile_width != 16 || tile_height != 16) {
        return false;
    }
    const __m256i mask = noise_word_mask();
    const __m256i four = _mm256_set1_epi16(4);
    const __m256i cap = _mm256_set1_epi16(256);
    for (int y = 1; y < 15; ++y) {
        const uint8_t* row =
            plane.data +
            static_cast<ptrdiff_t>(tile_y + y) *
                plane.stride_bytes +
            static_cast<ptrdiff_t>(tile_x) *
                plane.sample_step_bytes;
        const uint8_t* up = row - plane.stride_bytes;
        const uint8_t* down = row + plane.stride_bytes;
        const __m128i row_values =
            packed_u16
                ? load_normalized_u16(
                      row, plane.bit_depth, plane.sample_shift)
                : _mm_loadu_si128(
                      reinterpret_cast<const __m128i*>(row));
        const __m128i up_values =
            packed_u16
                ? load_normalized_u16(
                      up, plane.bit_depth, plane.sample_shift)
                : _mm_loadu_si128(
                      reinterpret_cast<const __m128i*>(up));
        const __m128i down_values =
            packed_u16
                ? load_normalized_u16(
                      down, plane.bit_depth, plane.sample_shift)
                : _mm_loadu_si128(
                      reinterpret_cast<const __m128i*>(down));
        const __m256i center = _mm256_cvtepu8_epi16(
            _mm_srli_si128(row_values, 1));
        const __m256i left =
            _mm256_cvtepu8_epi16(row_values);
        const __m256i right = _mm256_cvtepu8_epi16(
            _mm_srli_si128(row_values, 2));
        const __m256i up_center = _mm256_cvtepu8_epi16(
            _mm_srli_si128(up_values, 1));
        const __m256i down_center = _mm256_cvtepu8_epi16(
            _mm_srli_si128(down_values, 1));
        __m256i laplacian =
            _mm256_mullo_epi16(center, four);
        laplacian = _mm256_sub_epi16(laplacian, left);
        laplacian = _mm256_sub_epi16(laplacian, right);
        laplacian = _mm256_sub_epi16(laplacian, up_center);
        laplacian = _mm256_sub_epi16(laplacian, down_center);
        const __m256i clipped = _mm256_and_si256(
            _mm256_min_epi16(
                _mm256_abs_epi16(laplacian), cap),
            mask);
        residual_sum_times_four += sum_u16(clipped);
        residual_count += 14;
    }
    _mm256_zeroupper();
    return true;
}

bool banding_tile_avx2_u8(const LumaPlaneView& plane,
                          int tile_x,
                          int tile_y,
                          int tile_width,
                          int tile_height,
                          BandingTileStats& stats) {
    stats = BandingTileStats{};
    const bool packed_u16 = is_packed_u16(plane);
    if ((!is_packed_u8(plane) && !packed_u16) ||
        tile_width != 16 || tile_height != 16) {
        return false;
    }
    const auto& horizontal_mask_bytes =
        banding_horizontal_mask();
    const __m128i horizontal_mask = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(
            horizontal_mask_bytes.data()));
    const __m128i full_mask =
        _mm_set1_epi8(static_cast<char>(0xff));
    for (int y = 0; y < 16; ++y) {
        const uint8_t* row =
            plane.data +
            static_cast<ptrdiff_t>(tile_y + y) *
                plane.stride_bytes +
            static_cast<ptrdiff_t>(tile_x) *
                plane.sample_step_bytes;
        const __m128i row_values =
            packed_u16
                ? load_normalized_u16(
                      row, plane.bit_depth, plane.sample_shift)
                : _mm_loadu_si128(
                      reinterpret_cast<const __m128i*>(row));
        alignas(16) uint8_t normalized[16];
        _mm_store_si128(
            reinterpret_cast<__m128i*>(normalized),
            row_values);
        for (int x = 0; x < 16; ++x) {
            const uint8_t value = normalized[x];
            stats.present[value >> 6] |=
                uint64_t{1} << (value & 63);
            stats.minimum =
                std::min(stats.minimum, static_cast<int>(value));
            stats.maximum =
                std::max(stats.maximum, static_cast<int>(value));
        }

        const __m128i right_values =
            _mm_srli_si128(row_values, 1);
        const __m128i horizontal_differences =
            _mm_and_si128(
                _mm_or_si128(
                    _mm_subs_epu8(
                        row_values, right_values),
                    _mm_subs_epu8(
                        right_values, row_values)),
                horizontal_mask);
        stats.gradient_sum +=
            sum_16_bytes(horizontal_differences);
        stats.weak_contours += count_weak_differences(
            horizontal_differences, horizontal_mask);
        stats.edge_count += 15;

        if (y < 15) {
            const uint8_t* down = row + plane.stride_bytes;
            const __m128i down_values =
                packed_u16
                    ? load_normalized_u16(
                          down,
                          plane.bit_depth,
                          plane.sample_shift)
                    : _mm_loadu_si128(
                          reinterpret_cast<const __m128i*>(down));
            const __m128i vertical_differences =
                _mm_or_si128(
                    _mm_subs_epu8(row_values, down_values),
                    _mm_subs_epu8(down_values, row_values));
            stats.gradient_sum +=
                sum_16_bytes(vertical_differences);
            stats.weak_contours += count_weak_differences(
                vertical_differences, full_mask);
            stats.edge_count += 16;
        }
    }
    _mm256_zeroupper();
    return true;
}

}  // namespace vr::analysis::quality::cpu
