#include "windows/d3d11/frame_capture_service.h"

namespace vr {

bool FrameCaptureService::capture_headless_front_buffer(
    D3D11HeadlessOutput& output,
    std::recursive_mutex& device_mutex,
    std::vector<uint8_t>& bgra,
    int& width,
    int& height) const {
    bgra.clear();
    width = 0;
    height = 0;

    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex);
    D3D11HeadlessOutputFrontBufferSnapshot snapshot;
    {
        std::lock_guard<std::mutex> tex_lock(output.texture_mutex());
        if (!output.snapshot_front_buffer_locked(snapshot)) {
            return false;
        }
    }
    return output.capture_front_buffer_snapshot(snapshot, bgra, width, height);
}

bool FrameCaptureService::capture_headless_front_buffer_region(
    D3D11HeadlessOutput& output,
    std::recursive_mutex& device_mutex,
    int x,
    int y,
    int width,
    int height,
    std::vector<uint8_t>& bgra,
    int& region_width,
    int& region_height) const {
    bgra.clear();
    region_width = 0;
    region_height = 0;

    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex);
    D3D11HeadlessOutputFrontBufferSnapshot snapshot;
    {
        std::lock_guard<std::mutex> tex_lock(output.texture_mutex());
        if (!output.snapshot_front_buffer_locked(snapshot)) {
            return false;
        }
    }
    return output.capture_front_buffer_region_snapshot(
        snapshot, x, y, width, height, bgra, region_width, region_height);
}

} // namespace vr
