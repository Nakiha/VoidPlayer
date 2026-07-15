#pragma once

#include <cstring>

namespace vr {

struct RendererFrameRefreshPolicy {
    bool retain_presented_frame = false;
    bool decoded_preview_refresh = false;
    bool interaction_refresh = false;
};

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
