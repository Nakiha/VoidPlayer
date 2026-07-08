#include "renderer/render/presentation_source_cache_state.h"

namespace vr {

PresentationSourceCacheReadyState build_presentation_source_cache_ready_state(
    const PresentationBackendDiagnostics& backend,
    const PresentationSourceCacheCompositorState& compositor) {
    PresentationSourceCacheReadyState state;
    state.active = compositor.accepts_source_cache &&
        (backend.source_cache_active ||
         backend.source_cache_texture_count > 0 ||
         backend.source_cache_publish_count > 0);
    state.ready = state.active && backend.source_cache_publish_count > 0;
    state.frozen_snapshot = backend.source_cache_frozen_snapshot;
    state.texture_count = backend.source_cache_texture_count;
    state.generation = backend.source_cache_generation;
    state.publish_count = backend.source_cache_publish_count;
    state.consumed_generation = compositor.consumed_generation;
    state.fallback_count =
        backend.source_cache_fallback_count + compositor.fallback_count;
    state.backpressure_count = backend.source_cache_backpressure_count;
    state.required_mask = backend.source_cache_required_mask;
    state.drawn_mask = backend.source_cache_drawn_mask;
    state.missing_mask = backend.source_cache_missing_mask;
    state.incomplete_publish_suppressed_count =
        backend.source_cache_incomplete_publish_suppressed_count;
    state.format = backend.source_cache_format;
    state.last_error = compositor.last_error != "none"
        ? compositor.last_error
        : backend.source_cache_last_error;
    return state;
}

uint64_t build_presentation_source_cache_required_mask(size_t required_count) {
    if (required_count == 0) {
        return 0;
    }
    if (required_count >= 64) {
        return UINT64_MAX;
    }
    return (uint64_t{1} << required_count) - 1;
}

PresentationSourceCacheCommitState build_presentation_source_cache_commit_state(
    size_t required_count,
    uint64_t drawn_mask,
    const std::string& error) {
    PresentationSourceCacheCommitState state;
    state.required_mask =
        build_presentation_source_cache_required_mask(required_count);
    state.drawn_mask = drawn_mask & state.required_mask;
    state.missing_mask = state.required_mask & ~state.drawn_mask;
    state.complete = state.required_mask != 0 && state.missing_mask == 0 &&
        (error.empty() || error == "none");
    state.error = state.complete
        ? "none"
        : (!error.empty() && error != "none" ? error
                                             : "source-package-incomplete");
    return state;
}

} // namespace vr
