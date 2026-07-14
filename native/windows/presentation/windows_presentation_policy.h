#pragma once

#include <string>
#include <vector>

#include "renderer/color/color_strategy.h"
#include "windows/presentation/windows_display_resolver.h"

namespace vr {

struct TrackInfo;

struct WindowsPresentationPolicy {
  std::string request = "auto";
  std::string mode = "native-compositor-sdr";
  std::string reason = "auto-sdr-only";
  std::string fallback_reason = "none";
  ColorOutputTarget output_target = ColorOutputTarget::kSDRToneMappedBT709;
  bool auto_enabled = true;
  bool has_hdr_track = false;
  bool hdr_output_requested = false;
  bool supported = true;
};

WindowsPresentationPolicy resolve_windows_presentation_policy(
    const std::string& requested_mode, bool has_hdr_track,
    const WindowsDisplayProbeResult& display);

bool windows_tracks_have_hdr_transfer(const std::vector<TrackInfo>& tracks);

}  // namespace vr
