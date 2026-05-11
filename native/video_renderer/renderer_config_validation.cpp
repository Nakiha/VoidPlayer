#include "video_renderer/renderer_config_validation.h"
#include "common/win_utf8.h"

#include <cmath>
#include <utility>

namespace vr {
namespace {

RendererConfigValidationResult ok_result() {
    return {};
}

RendererConfigValidationResult invalid(std::string message) {
    RendererConfigValidationResult result;
    result.ok = false;
    result.code = RendererConfigValidationCode::InvalidArgument;
    result.message = std::move(message);
    return result;
}

} // namespace

RendererConfigValidationResult validate_renderer_dimensions(
    int width,
    int height,
    const char* label) {
    if (width <= 0 || height <= 0) {
        return invalid(std::string(label ? label : "renderer dimensions") +
                       " must be positive");
    }
    const auto budget = default_native_resource_budget();
    if (width > budget.max_dimension || height > budget.max_dimension) {
        return invalid(std::string(label ? label : "renderer dimensions") +
                       " exceed maximum dimension");
    }
    return ok_result();
}

RendererConfigValidationResult validate_renderer_video_path(
    const std::string& path) {
    if (path.empty()) {
        return invalid("video path must not be empty");
    }
    const auto budget = default_native_resource_budget();
    if (path.size() > budget.max_path_bytes) {
        return invalid("video path exceeds maximum byte length");
    }
    if (path.find('\0') != std::string::npos) {
        return invalid("video path must not contain embedded null bytes");
    }
    if (!win_utf8::is_valid_utf8(path)) {
        return invalid("video path must be valid UTF-8");
    }
    return ok_result();
}

RendererConfigValidationResult validate_renderer_config(
    const RendererConfig& config) {
    if (auto result = validate_renderer_dimensions(config.width, config.height);
        !result.ok) {
        return result;
    }

    if (config.video_paths.empty()) {
        return invalid("at least one video path is required");
    }
    const auto budget = default_native_resource_budget();
    if (config.video_paths.size() > budget.max_tracks) {
        return invalid("too many video paths");
    }
    for (const auto& path : config.video_paths) {
        if (auto result = validate_renderer_video_path(path); !result.ok) {
            return result;
        }
    }

    if (config.headless) {
        if (config.hwnd != nullptr) {
            return invalid("headless renderer must not also receive an HWND");
        }
        if (config.backend.type != RendererBackendType::D3D11) {
            return invalid("unsupported renderer backend type");
        }
        if (config.backend.adapter == nullptr) {
            return invalid("headless renderer requires a DXGI adapter");
        }
    } else if (config.hwnd == nullptr) {
        return invalid("windowed renderer requires an HWND");
    }

    return ok_result();
}

RendererConfigValidationResult validate_playback_speed(double speed) {
    const auto budget = default_native_resource_budget();
    if (!std::isfinite(speed) || speed <= 0.0 || speed > budget.max_playback_speed) {
        return invalid("playback speed out of range");
    }
    return ok_result();
}

RendererConfigValidationResult validate_loop_range(
    bool enabled,
    int64_t start_us,
    int64_t end_us) {
    if (!enabled) {
        return ok_result();
    }
    if (start_us < 0 || end_us <= start_us) {
        return invalid("loop range must satisfy 0 <= start < end");
    }
    return ok_result();
}

} // namespace vr
