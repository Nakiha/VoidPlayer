#pragma once

#include "playback/playback_controller.h"
#include "renderer/renderer.h"
#include <functional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace vr {

struct SharedFp16TextureSnapshot;

/// Native player facade that owns playback control and the video renderer as
/// peers. FFI can adopt this type without changing the renderer/video internals.
class NativePlayer {
public:
    NativePlayer();
    ~NativePlayer();

    NativePlayer(const NativePlayer&) = delete;
    NativePlayer& operator=(const NativePlayer&) = delete;

    bool initialize(const RendererConfig& config);
    void shutdown();

    void play();
    void pause();
    void seek(int64_t target_pts_us,
              SeekType type = SeekType::Keyframe,
              int64_t request_id = -1);
    void set_speed(double speed);
    void set_loop_range(bool enabled, int64_t start_us, int64_t end_us);
    void set_audible_track(int file_id);
    int audible_track() const;

    void step_forward();
    void step_backward();

    bool is_playing() const;
    bool is_initialized() const;
    int64_t current_pts_us() const;
    double current_speed() const;
    size_t track_count() const;
    int64_t duration_us() const;

    int add_track(const std::string& video_path, bool use_hardware_decode = true);
    void remove_track(int file_id);
    bool has_track(int slot) const;
    std::pair<int, int> track_dimensions(int slot) const;
    std::vector<TrackInfo> track_infos() const;
    std::vector<TrackPerfStats> track_perf_stats() const;
    RendererGpuMemoryStats gpu_memory_stats() const;
    PresentationBackendDiagnostics presentation_backend_diagnostics() const;
    AudioOutputStats audio_output_stats() const;
    bool d3d_device_lost() const;
    long d3d_device_removed_reason() const;
    void set_track_offset(int file_id, int64_t offset_us);

    void apply_layout(const LayoutState& state);
    void set_background_color(float r, float g, float b, float a);
    LayoutState layout() const;

    void set_frame_callback(std::function<void()> cb);
    void set_event_callback(RendererEventCallback cb);
    int texture_width() const;
    int texture_height() const;
    bool acquire_shared_texture(SharedTextureSnapshot& snapshot) const;
    void release_shared_texture(int buffer_index, uint64_t buffer_generation) const;
    bool acquire_shared_fp16_texture(SharedFp16TextureSnapshot& snapshot) const;
    void release_shared_fp16_texture(int buffer_index, uint64_t ring_generation) const;
    void set_shared_fp16_frame_callback(std::function<void()> cb);
    void resize(int width, int height);
    bool capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height);
    bool capture_front_buffer_region(int x,
                                     int y,
                                     int width,
                                     int height,
                                     std::vector<uint8_t>& bgra,
                                     int& region_width,
                                     int& region_height);

    PlaybackController& playback() { return playback_; }
    const PlaybackController& playback() const { return playback_; }
    Renderer& renderer() { return renderer_; }
    const Renderer& renderer() const { return renderer_; }

private:
    enum class State {
        Created,
        Initializing,
        Initialized,
        ShuttingDown,
    };

    bool renderer_ready_locked() const;

    mutable std::shared_mutex lifecycle_mutex_;
    State state_ = State::Created;
    PlaybackController playback_;
    Renderer renderer_;
};

} // namespace vr
