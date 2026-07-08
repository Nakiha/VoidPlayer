#pragma once

#include "renderer/render/presentation_backend_types.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace vr {

struct PresentationSourceCacheCompositorState {
    bool accepts_source_cache = false;
    uint64_t consumed_generation = 0;
    uint64_t fallback_count = 0;
    std::string last_error = "none";
};

struct PresentationSourceCacheReadyState {
    bool active = false;
    bool ready = false;
    bool frozen_snapshot = false;
    int32_t texture_count = 0;
    uint64_t generation = 0;
    uint64_t publish_count = 0;
    uint64_t consumed_generation = 0;
    uint64_t fallback_count = 0;
    uint64_t backpressure_count = 0;
    uint64_t required_mask = 0;
    uint64_t drawn_mask = 0;
    uint64_t missing_mask = 0;
    uint64_t incomplete_publish_suppressed_count = 0;
    std::string format = "R16G16B16A16_FLOAT";
    std::string last_error = "none";
};

struct PresentationSourceCacheCommitState {
    bool complete = false;
    uint64_t required_mask = 0;
    uint64_t drawn_mask = 0;
    uint64_t missing_mask = 0;
    std::string error = "none";
};

PresentationSourceCacheReadyState build_presentation_source_cache_ready_state(
    const PresentationBackendDiagnostics& backend,
    const PresentationSourceCacheCompositorState& compositor);

uint64_t build_presentation_source_cache_required_mask(size_t required_count);

PresentationSourceCacheCommitState build_presentation_source_cache_commit_state(
    size_t required_count,
    uint64_t drawn_mask,
    const std::string& error = "none");

} // namespace vr
