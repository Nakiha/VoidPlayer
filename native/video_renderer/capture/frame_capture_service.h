#pragma once

#include "video_renderer/d3d11/headless_output.h"
#include <mutex>
#include <vector>

namespace vr {

/// Native-facing front-buffer capture boundary.
///
/// The service owns the lock choreography for D3D11 headless captures:
/// immediate-context work is serialized by device_mutex, while
/// D3D11HeadlessOutput::texture_mutex() is held only long enough to pin the
/// current front-buffer snapshot.
class FrameCaptureService {
public:
    bool capture_headless_front_buffer(D3D11HeadlessOutput& output,
                                       std::recursive_mutex& device_mutex,
                                       std::vector<uint8_t>& bgra,
                                       int& width,
                                       int& height) const;
};

} // namespace vr
