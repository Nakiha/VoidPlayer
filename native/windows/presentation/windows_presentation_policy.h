#pragma once

#include "renderer/color/color_strategy.h"

#include <string>

namespace vr {

struct WindowsPresentationPolicy {
    std::string request = "sdr";
    std::string mode = "flutter-texture-sdr";
    std::string reason = "fixed-sdr-current-route";
    std::string fallback_reason = "none";
    ColorOutputTarget output_target = ColorOutputTarget::kSDRToneMappedBT709;
    bool fp16_scrgb_requested = false;
};

WindowsPresentationPolicy resolve_windows_presentation_policy(
    const std::string& requested_mode);

} // namespace vr
