#pragma once

#include "renderer/color/color_strategy.h"
#include "windows/presentation/windows_display_resolver.h"

#include <string>
#include <vector>

namespace vr {

struct TrackInfo;

struct WindowsPresentationPolicy {
    std::string request = "auto";
    std::string mode = "native-compositor-sdr";
    std::string desired_mode = "native-compositor-sdr";
    std::string reason = "auto-sdr-only";
    std::string fallback_reason = "none";
    ColorOutputTarget output_target = ColorOutputTarget::kWindowsLinearScRGB;
    bool auto_enabled = true;
    bool has_hdr_track = false;
    bool fp16_scrgb_requested = true;
    bool native_compositor_requested = true;
    bool hdr_output_requested = false;
    bool cross_adapter_required = false;
    bool cross_adapter_migration_requested = false;
};

WindowsPresentationPolicy resolve_windows_presentation_policy(
    const std::string& requested_mode,
    bool has_hdr_track,
    const WindowsDisplayProbeResult& display);

WindowsPresentationPolicy resolve_windows_presentation_policy(
    const std::string& requested_mode);

bool windows_tracks_have_hdr_transfer(
    const std::vector<TrackInfo>& tracks);

} // namespace vr
