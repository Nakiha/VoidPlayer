#pragma once

#include "renderer/renderer_api_types.h"

#include <cstring>

namespace vr {

struct RendererFrameRefreshPolicy {
    bool retain_presented_frame = false;
    bool decoded_preview_refresh = false;
    bool interaction_refresh = false;
};

enum class RendererInteractionRefreshDisposition : uint8_t {
    Presented,
    RetryNotReady,
    RetryBackpressure,
    Failed,
};

inline RendererInteractionRefreshDisposition
classify_interaction_refresh_result(
    RendererFrameRefreshResult result,
    bool transient_backend_backpressure) {
    if (result == RendererFrameRefreshResult::Presented) {
        return RendererInteractionRefreshDisposition::Presented;
    }
    if (result == RendererFrameRefreshResult::NotReady) {
        return RendererInteractionRefreshDisposition::RetryNotReady;
    }
    return transient_backend_backpressure
        ? RendererInteractionRefreshDisposition::RetryBackpressure
        : RendererInteractionRefreshDisposition::Failed;
}

inline RendererFrameRefreshPolicy renderer_frame_refresh_policy(
    const char* reason) {
    const char* refresh_reason =
        reason && reason[0] != '\0' ? reason : "request_frame_refresh";
    const bool interaction_refresh =
        std::strcmp(refresh_reason, "renderer-owned-interaction-refresh") == 0;
    const bool retain_presented_frame =
        interaction_refresh ||
        std::strcmp(refresh_reason, "macos-renderer-owned-refresh") == 0 ||
        std::strcmp(refresh_reason, "request_frame_refresh") == 0 ||
        std::strcmp(refresh_reason, "windows-analysis-overlay-state") == 0;
    return RendererFrameRefreshPolicy{
        retain_presented_frame,
        std::strcmp(refresh_reason, "seek_frame_refresh") == 0,
        interaction_refresh,
    };
}

} // namespace vr
