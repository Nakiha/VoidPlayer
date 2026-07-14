#include "windows_presentation_policy.h"

#include <algorithm>
#include <cctype>

#include "renderer/frame/frame_storage.h"
#include "renderer/track/track_info.h"

namespace vr {
namespace {

std::string normalized_mode(std::string value) {
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [](unsigned char ch) { return !std::isspace(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [](unsigned char ch) { return !std::isspace(ch); })
                  .base(),
              value.end());
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

WindowsPresentationPolicy forced_sdr(std::string request, bool has_hdr_track) {
  WindowsPresentationPolicy policy;
  policy.request = std::move(request);
  policy.reason = "forced-native-compositor-sdr";
  policy.auto_enabled = false;
  policy.has_hdr_track = has_hdr_track;
  return policy;
}

}  // namespace

WindowsPresentationPolicy resolve_windows_presentation_policy(
    const std::string& requested_mode, bool has_hdr_track,
    const WindowsDisplayProbeResult& display) {
  std::string request = normalized_mode(requested_mode);
  if (request.empty()) {
    request = "auto";
  }
  if (request == "sdr" || request == "native-compositor-sdr") {
    return forced_sdr(request, has_hdr_track);
  }
  if (request == "native-compositor-scrgb" || request == "scrgb" ||
      request == "hdr") {
    WindowsPresentationPolicy policy;
    policy.request = request;
    policy.mode = "native-compositor-scrgb";
    policy.reason = "forced-native-compositor-scrgb";
    policy.output_target = ColorOutputTarget::kWindowsLinearScRGB;
    policy.auto_enabled = false;
    policy.has_hdr_track = has_hdr_track;
    policy.hdr_output_requested = true;
    if (display.output_resolved && !display.matches_presentation_adapter) {
      policy.mode = "native-compositor-sdr";
      policy.reason = "forced-scrgb-cross-adapter-unsupported";
      policy.fallback_reason = "cross-adapter-output-unsupported";
      policy.output_target = ColorOutputTarget::kSDRToneMappedBT709;
      policy.hdr_output_requested = false;
    }
    return policy;
  }
  if (request == "auto") {
    WindowsPresentationPolicy policy;
    policy.request = request;
    policy.has_hdr_track = has_hdr_track;
    if (!has_hdr_track) {
      return policy;
    }
    if (!display.output_resolved || !display.color_metadata_available ||
        !display.hdr_active) {
      policy.reason = "auto-hdr-display-unavailable";
      return policy;
    }
    if (!display.matches_presentation_adapter) {
      policy.reason = "auto-hdr-cross-adapter-unsupported";
      policy.fallback_reason = "cross-adapter-output-unsupported";
      return policy;
    }
    policy.mode = "native-compositor-scrgb";
    policy.reason = "auto-hdr-track";
    policy.output_target = ColorOutputTarget::kWindowsLinearScRGB;
    policy.hdr_output_requested = true;
    return policy;
  }
  WindowsPresentationPolicy policy;
  policy.request = request;
  policy.mode = "unsupported";
  policy.reason = "unsupported-windows-presentation-mode";
  policy.fallback_reason = "unsupported-windows-presentation-mode";
  policy.auto_enabled = false;
  policy.has_hdr_track = has_hdr_track;
  policy.supported = false;
  return policy;
}

bool windows_tracks_have_hdr_transfer(const std::vector<TrackInfo>& tracks) {
  return std::any_of(tracks.begin(), tracks.end(), [](const TrackInfo& track) {
    return track.color.transfer == VIDEO_COLOR_TRANSFER_PQ ||
           track.color.transfer == VIDEO_COLOR_TRANSFER_HLG;
  });
}

}  // namespace vr
