#include "windows_presentation_policy.h"

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
    const std::string& requested_mode) {
    const std::string request = normalized_mode(requested_mode);
    if (request.empty() || request == "sdr") {
        return {};
    }
    if (request == "fp16-scrgb") {
        WindowsPresentationPolicy policy;
        policy.request = "fp16-scrgb";
        policy.mode = "flutter-texture-sdr-fp16-scrgb";
        policy.reason = "forced-fp16-scrgb";
        policy.output_target = ColorOutputTarget::kWindowsLinearScRGB;
        policy.fp16_scrgb_requested = true;
        return policy;
    }

    WindowsPresentationPolicy policy;
    policy.request = request;
    policy.reason = "unsupported-presentation-request";
    policy.fallback_reason = "unsupported-presentation-request";
    return policy;
}

} // namespace vr
