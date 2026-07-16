#pragma once

#include <cstdint>

namespace vr {

inline uint64_t decode_active_batch_time_us(
    uint64_t elapsed_us,
    uint64_t publish_wait_before_us,
    uint64_t publish_wait_after_us) {
    const uint64_t publish_wait_us =
        publish_wait_after_us >= publish_wait_before_us
            ? publish_wait_after_us - publish_wait_before_us
            : 0;
    return elapsed_us > publish_wait_us ? elapsed_us - publish_wait_us : 0;
}

}  // namespace vr
