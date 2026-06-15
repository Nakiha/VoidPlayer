#include "windows_presentation_policy.h"

#include "renderer/frame/frame_storage.h"
#include "renderer/track/track_info.h"

#include <algorithm>
#include <cctype>

namespace vr {
namespace {

std::string normalized_mode(std::string value) {
    value.erase(
        value.begin(),
        std::find_if(value.begin(), value.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(),
        value.end());
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

} // namespace

WindowsPresentationPolicy resolve_windows_presentation_policy(
    const std::string& requested_mode,
    bool has_hdr_track,
    const WindowsDisplayProbeResult& display) {
    const std::string request = normalized_mode(requested_mode);
    if (request.empty() || request == "auto") {
        WindowsPresentationPolicy policy;
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
            policy.reason = "auto-hdr-adapter-mismatch";
            return policy;
        }
        policy.mode = "native-compositor-scrgb";
        policy.desired_mode = policy.mode;
        policy.reason = "auto-hdr-track";
        policy.hdr_output_requested = true;
        return policy;
    }
    if (request == "sdr" || request == "flutter-texture-sdr") {
        WindowsPresentationPolicy policy;
        policy.request = request;
        policy.mode = "flutter-texture-sdr";
        policy.desired_mode = policy.mode;
        policy.reason = "forced-flutter-texture-sdr";
        policy.output_target = ColorOutputTarget::kSDRToneMappedBT709;
        policy.auto_enabled = false;
        policy.has_hdr_track = has_hdr_track;
        policy.fp16_scrgb_requested = false;
        policy.native_compositor_requested = false;
        return policy;
    }
    if (request == "fp16-scrgb") {
        WindowsPresentationPolicy policy;
        policy.request = "fp16-scrgb";
        policy.mode = "flutter-texture-sdr-fp16-scrgb";
        policy.reason = "forced-fp16-scrgb";
        policy.output_target = ColorOutputTarget::kWindowsLinearScRGB;
        policy.auto_enabled = false;
        policy.has_hdr_track = has_hdr_track;
        policy.fp16_scrgb_requested = true;
        policy.native_compositor_requested = false;
        return policy;
    }
    if (request == "native-compositor-sdr") {
        WindowsPresentationPolicy policy;
        policy.request = request;
        policy.mode = request;
        policy.desired_mode = request;
        policy.reason = "forced-native-compositor-sdr";
        policy.auto_enabled = false;
        policy.has_hdr_track = has_hdr_track;
        return policy;
    }
    if (request == "native-compositor-scrgb") {
        WindowsPresentationPolicy policy;
        policy.request = request;
        policy.mode = "native-compositor-scrgb";
        policy.desired_mode = policy.mode;
        policy.reason = "forced-native-compositor-scrgb";
        policy.output_target = ColorOutputTarget::kWindowsLinearScRGB;
        policy.auto_enabled = false;
        policy.has_hdr_track = has_hdr_track;
        policy.fp16_scrgb_requested = true;
        policy.native_compositor_requested = true;
        policy.hdr_output_requested = true;
        return policy;
    }

    WindowsPresentationPolicy policy;
    policy.request = request;
    policy.mode = "flutter-texture-sdr";
    policy.desired_mode = policy.mode;
    policy.reason = "unsupported-presentation-request";
    policy.fallback_reason = "unsupported-presentation-request";
    policy.output_target = ColorOutputTarget::kSDRToneMappedBT709;
    policy.auto_enabled = false;
    policy.has_hdr_track = has_hdr_track;
    policy.fp16_scrgb_requested = false;
    policy.native_compositor_requested = false;
    return policy;
}

WindowsPresentationPolicy resolve_windows_presentation_policy(
    const std::string& requested_mode) {
    return resolve_windows_presentation_policy(
        requested_mode, false, WindowsDisplayProbeResult{});
}

bool windows_tracks_have_hdr_transfer(
    const std::vector<TrackInfo>& tracks) {
    return std::any_of(
        tracks.begin(), tracks.end(), [](const TrackInfo& track) {
            return track.color.transfer == VIDEO_COLOR_TRANSFER_PQ ||
                   track.color.transfer == VIDEO_COLOR_TRANSFER_HLG;
        });
}

} // namespace vr
