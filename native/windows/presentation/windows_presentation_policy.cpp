#include "windows_presentation_policy.h"

#include "renderer/frame/frame_storage.h"
#include "renderer/render/presentation_color_domain_contract.h"
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

PresentationFlutterSurfaceColorContract evaluate_windows_auto_hdr_ui_contract() {
    PresentationBackendDiagnostics diagnostics;
    diagnostics.external_flutter_surface_color_domain =
        "sdr-srgb-premultiplied-bgra8";
    diagnostics.external_flutter_surface_composition_owner = "native-shader";
    diagnostics.external_flutter_surface_target_domain =
        "windows-linear-scrgb";
    diagnostics.external_flutter_surface_composited_into_hdr_target = true;
    return evaluate_flutter_surface_color_contract(diagnostics);
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
        policy.desired_mode = "native-compositor-scrgb";
        const auto ui_contract = evaluate_windows_auto_hdr_ui_contract();
        if (ui_contract.violation) {
            policy.reason = "auto-hdr-ui-composition-unsupported";
            policy.fallback_reason = "hdr-ui-composition-unsupported";
            return policy;
        }
        policy.mode = policy.desired_mode;
        policy.reason = "auto-hdr-ui-composition-owned";
        policy.output_target = ColorOutputTarget::kWindowsLinearScRGB;
        policy.fp16_scrgb_requested = true;
        policy.hdr_output_requested = true;
        return policy;
    }
    if (request == "sdr") {
        WindowsPresentationPolicy policy;
        policy.request = request;
        policy.mode = "native-compositor-sdr";
        policy.desired_mode = policy.mode;
        policy.reason = "forced-native-compositor-sdr";
        policy.auto_enabled = false;
        policy.has_hdr_track = has_hdr_track;
        return policy;
    }
    if (request == "flutter" || request == "flutter-texture-sdr" ||
        request == "fp16-scrgb") {
        WindowsPresentationPolicy policy;
        policy.request = request;
        policy.mode = "unsupported";
        policy.desired_mode = "unsupported";
        policy.reason = "unsupported-windows-presentation-mode";
        policy.fallback_reason = "unsupported-windows-presentation-mode";
        policy.auto_enabled = false;
        policy.has_hdr_track = has_hdr_track;
        policy.fp16_scrgb_requested = false;
        policy.native_compositor_requested = false;
        policy.supported = false;
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
        policy.cross_adapter_required =
            display.output_resolved && !display.matches_presentation_adapter;
        policy.cross_adapter_migration_requested =
            policy.cross_adapter_required;
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
