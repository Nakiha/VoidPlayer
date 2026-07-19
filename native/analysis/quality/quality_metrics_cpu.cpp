#include "analysis/quality/quality_metrics_cpu.h"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace vr::analysis::quality::cpu {

bool avx2_is_available() {
#if !defined(VOID_QUALITY_HAS_AVX2_KERNEL)
    return false;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int registers[4] = {};
    __cpuid(registers, 0);
    if (registers[0] < 7) {
        return false;
    }
    __cpuidex(registers, 1, 0);
    constexpr int kOsxsave = 1 << 27;
    constexpr int kAvx = 1 << 28;
    if ((registers[2] & (kOsxsave | kAvx)) != (kOsxsave | kAvx) ||
        (_xgetbv(0) & 0x6) != 0x6) {
        return false;
    }
    __cpuidex(registers, 7, 0);
    return (registers[1] & (1 << 5)) != 0;
#elif (defined(__GNUC__) || defined(__clang__)) && \
      (defined(__x86_64__) || defined(__i386__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

#if !defined(VOID_QUALITY_HAS_AVX2_KERNEL)
bool blockiness_period_avx2_u8(const LumaPlaneView&,
                              int,
                              BlockinessPeriodSums&) {
    return false;
}

bool blur_direction_avx2_u8(const LumaPlaneView&,
                            bool,
                            BlurDirectionSums&) {
    return false;
}

bool noise_tile_gradient_avx2_u8(const LumaPlaneView&,
                                 int,
                                 int,
                                 int,
                                 int,
                                 uint64_t&,
                                 uint64_t&) {
    return false;
}

bool noise_tile_residual_avx2_u8(const LumaPlaneView&,
                                 int,
                                 int,
                                 int,
                                 int,
                                 uint64_t&,
                                 uint64_t&) {
    return false;
}

bool banding_tile_avx2_u8(const LumaPlaneView&,
                          int,
                          int,
                          int,
                          int,
                          BandingTileStats&) {
    return false;
}
#endif

}  // namespace vr::analysis::quality::cpu
