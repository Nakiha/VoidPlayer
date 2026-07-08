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
    (void)display;
    if (request.empty() || request == "auto") {
        WindowsPresentationPolicy policy;
        policy.has_hdr_track = has_hdr_track;
        if (has_hdr_track) {
            policy.reason = "auto-hdr-deferred-flutter-texture-sdr";
            policy.fallback_reason = "hdr-native-compositor-deferred";
        }
        return policy;
    }
    if (request == "sdr" || request == "flutter" ||
        request == "flutter-texture-sdr") {
        WindowsPresentationPolicy policy;
        policy.request = request;
        policy.mode = "flutter-texture-sdr";
        policy.desired_mode = policy.mode;
        policy.reason = "forced-flutter-texture-sdr";
        policy.auto_enabled = false;
        policy.has_hdr_track = has_hdr_track;
        return policy;
    }
    if (request == "fp16-scrgb" || request == "native-compositor-sdr" ||
        request == "native-compositor-scrgb") {
        WindowsPresentationPolicy policy;
        policy.request = request;
        policy.mode = "unsupported";
        policy.desired_mode = "unsupported";
        policy.reason = "native-compositor-deferred";
        policy.fallback_reason = "native-compositor-deferred";
        policy.auto_enabled = false;
        policy.has_hdr_track = has_hdr_track;
        policy.fp16_scrgb_requested = false;
        policy.native_compositor_requested = false;
        policy.supported = false;
        return policy;
    }

    WindowsPresentationPolicy policy;
    policy.request = request;
    policy.mode = "unsupported";
    policy.desired_mode = policy.mode;
    policy.reason = "unsupported-windows-presentation-mode";
    policy.fallback_reason = "unsupported-windows-presentation-mode";
    policy.auto_enabled = false;
    policy.has_hdr_track = has_hdr_track;
    policy.fp16_scrgb_requested = false;
    policy.native_compositor_requested = false;
    policy.supported = false;
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
