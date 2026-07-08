#pragma once

#include "renderer/render/presentation_backend_types.h"

#include <array>
#include <string>

namespace vr {

bool validate_presentation_source_projection(
    const PresentationSourceProjection& projection,
    const std::array<bool, 4>& source_present,
    std::string* error = nullptr);

PresentationSourceProjectionSample project_presentation_source_sample(
    float video_u,
    float video_v,
    const PresentationSourceProjection& projection,
    const std::array<bool, 4>& source_present);

std::array<PresentationRetainedSourceVisualRect, 4>
project_presentation_retained_source_visuals(
    float viewport_left,
    float viewport_top,
    float viewport_right,
    float viewport_bottom,
    const PresentationSourceProjection& projection,
    const std::array<bool, 4>& source_present);

} // namespace vr
