#include "renderer/renderer_config_validation.h"
#include "common/win_utf8.h"

#include <cmath>
#include <utility>

namespace vr {
namespace {

RendererConfigValidationResult ok_result() {
    return native_ok();
}

RendererConfigValidationResult invalid(std::string message) {
    return native_error(RendererConfigValidationCode::InvalidArgument,
                        std::move(message));
}

RendererConfigValidationResult validate_offscreen_backend(
    const RendererBackendInterop& backend) {
    const auto budget = default_native_resource_budget();
    switch (backend.type) {
    case RendererBackendType::Metal:
#ifdef __APPLE__
        if (backend.output == nullptr) {
            return invalid("offscreen metal renderer requires an output target");
        }
        if (backend.max_track_slots < 0 ||
            static_cast<size_t>(backend.max_track_slots) > budget.max_tracks) {
            return invalid("offscreen metal renderer max track slots out of range");
        }
        return ok_result();
#else
        return invalid("metal renderer is only supported on macOS");
#endif
    case RendererBackendType::NativeD3D11:
#ifdef _WIN32
        return invalid(
            "windows native-d3d11 renderer backend is reserved for the "
            "runner-composed sandwich path and is not implemented yet");
#else
        return invalid("native-d3d11 renderer is only supported on Windows");
#endif
    case RendererBackendType::NativeD3D12:
#ifdef _WIN32
        return invalid(
            "windows native-d3d12 renderer backend is reserved for the "
            "runner-composed sandwich path and is not implemented yet");
#else
        return invalid("native-d3d12 renderer is only supported on Windows");
#endif
    case RendererBackendType::Unknown:
    case RendererBackendType::Vulkan:
        return invalid("unsupported renderer backend type");
    }
    return invalid("unsupported renderer backend type");
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
    if (config.initial_file_id < 0) {
        return invalid("initial file id must not be negative");
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

    if (config.offscreen) {
        if (config.hwnd != nullptr) {
            return invalid("offscreen renderer must not also receive an HWND");
        }
        if (auto result = validate_offscreen_backend(config.backend); !result.ok) {
            return result;
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
    return validate_loop_range(
        enabled,
        media_time_from_us(start_us),
        media_time_from_us(end_us));
}

RendererConfigValidationResult validate_loop_range(
    bool enabled,
    MediaTime start,
    MediaTime end) {
    if (!enabled) {
        return ok_result();
    }
    if (media_time_us(start) < 0 || end <= start) {
        return invalid("loop range must satisfy 0 <= start < end");
    }
    return ok_result();
}

} // namespace vr
