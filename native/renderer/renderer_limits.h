#pragma once

#include <cstddef>

namespace vr {

constexpr int kMaxRendererDimension = 16384;
constexpr size_t kMaxRendererVideoPaths = 4;
constexpr size_t kMaxRendererPathBytes = 32767 * 4;
constexpr size_t kMaxCpuFrameBytes = size_t{1024} * 1024 * 1024;
constexpr size_t kMaxCaptureFrameBytes = size_t{1024} * 1024 * 1024;
constexpr size_t kMaxExactSeekReorderFrames = 128;
constexpr size_t kDefaultPacketQueueCapacity = 100;
constexpr size_t kHighResolutionTrackPixels = size_t{3840} * 2160;
constexpr size_t kDefaultTrackForwardDepth = 4;
constexpr size_t kDefaultTrackBackwardDepth = 1;
constexpr size_t kHighResolutionHardwareTrackForwardDepth = 1;
constexpr double kMaxPlaybackSpeed = 16.0;

struct NativeResourceBudget {
    size_t max_tracks = kMaxRendererVideoPaths;
    int max_dimension = kMaxRendererDimension;
    size_t max_path_bytes = kMaxRendererPathBytes;
    size_t max_cpu_frame_bytes = kMaxCpuFrameBytes;
    size_t max_capture_frame_bytes = kMaxCaptureFrameBytes;
    size_t max_exact_seek_reorder_frames = kMaxExactSeekReorderFrames;
    size_t packet_queue_capacity = kDefaultPacketQueueCapacity;
    size_t high_resolution_track_pixels = kHighResolutionTrackPixels;
    size_t default_track_forward_depth = kDefaultTrackForwardDepth;
    size_t default_track_backward_depth = kDefaultTrackBackwardDepth;
    size_t high_resolution_hardware_track_forward_depth = kHighResolutionHardwareTrackForwardDepth;
    double max_playback_speed = kMaxPlaybackSpeed;
};

constexpr NativeResourceBudget default_native_resource_budget() {
    return {};
}

} // namespace vr
