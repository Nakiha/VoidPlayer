#pragma once

#include "renderer/renderer_config.h"
#include "renderer/renderer_limits.h"

#include <cstdint>
#include <string>

namespace vr {

enum class RendererConfigValidationCode {
    Ok = 0,
    InvalidArgument,
};

struct RendererConfigValidationResult {
    bool ok = true;
    RendererConfigValidationCode code = RendererConfigValidationCode::Ok;
    std::string message;

    explicit operator bool() const { return ok; }
};

RendererConfigValidationResult validate_renderer_dimensions(
    int width,
    int height,
    const char* label = "renderer dimensions");

RendererConfigValidationResult validate_renderer_video_path(
    const std::string& path);

RendererConfigValidationResult validate_renderer_config(
    const RendererConfig& config);

RendererConfigValidationResult validate_playback_speed(double speed);

RendererConfigValidationResult validate_loop_range(
    bool enabled,
    int64_t start_us,
    int64_t end_us);

} // namespace vr
