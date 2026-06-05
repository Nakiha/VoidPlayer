#pragma once

#include "renderer/time/media_timestamp_constants.h"

#include <cstdint>

namespace vr {

constexpr int32_t kInvalidAnalysisFrameIndex = -1;
constexpr int32_t kInvalidSourcePacketIndex = -1;
constexpr int64_t kUnknownSourcePacketPosition = -1;

enum class FrameIdentityMode : int32_t {
    Unknown = 0,
    RuntimeOrdinal = 1,
    TimestampEstimated = 2,
    SourcePacketIdentity = 3,
    ExactAnalysisFrame = 4,
};

} // namespace vr
