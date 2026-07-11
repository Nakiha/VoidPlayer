#pragma once

#include "audio/audio_output_stats.h"
#include "media/seek_controller.h"
#include "renderer/layout/layout_state.h"
#include "renderer/render/presentation_backend_types.h"
#include "renderer/render/renderer_present_history.h"
#include "renderer/render/renderer_device_state.h"
#include "renderer/renderer_api_types.h"
#include "renderer/renderer_config.h"
#include "renderer/track/track_info.h"
#include "renderer/track/track_perf_stats.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vr {

class PlaybackController;
class PresentationBackend;

class Renderer {
public:
    Renderer();
    explicit Renderer(PlaybackController& playback);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

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
    bool has_audio() const;
    int audio_sample_rate() const;
    int audio_channels() const;
    AudioOutputStats audio_output_stats() const;

    void step_forward();
    void step_backward();

    bool is_playing() const;
    bool is_initialized() const;
    int64_t current_pts_us() const;
    double current_speed() const;

    size_t track_count() const;
    int64_t duration_us() const;

    int add_track(const std::string& video_path,
                  bool use_hardware_decode = true);
    int add_track_with_file_id(const std::string& video_path,
                               int file_id,
                               bool use_hardware_decode = true);
    void remove_track(int file_id);
    bool has_track(int slot) const;
    std::pair<int, int> track_dimensions(int slot) const;
    std::vector<TrackInfo> track_infos() const;
    std::vector<TrackPerfStats> track_perf_stats() const;
    RendererPresentedAnchorDiagnostics presented_anchor_diagnostics() const;

    PresentationBackendMetrics presentation_backend_metrics() const;
    PresentationBackendStats presentation_backend_stats() const;
    PresentationBackendDiagnostics presentation_backend_diagnostics() const;
    std::string presentation_backend_last_error() const;
    bool copy_last_presentation_frame_info(PresentationBackendFrameInfo* out) const;
    RendererGpuMemoryStats gpu_memory_stats() const;

    RendererDeviceState device_state() const;

    void set_track_offset(int file_id, int64_t offset_us);
    int64_t track_offset_us(int file_id) const;

    void apply_layout(const LayoutState& state);
    void set_background_color(float r, float g, float b, float a);
    LayoutState layout() const;

    void set_frame_callback(RendererFrameCallback cb);
    void set_frame_failure_callback(std::function<void(const char*)> cb);
    void set_event_callback(RendererEventCallback cb);

    int texture_width() const;
    int texture_height() const;

    bool prewarm_presentation_target(int width, int height);
    void resize(int width, int height);
    bool update_headless_output(void* output,
                                int width,
                                int height,
                                int max_track_slots);
    bool install_headless_output(void* output,
                                 int width,
                                 int height,
                                 int max_track_slots);
    bool install_headless_output_ring(const void* const* pixel_buffers,
                                      size_t pixel_buffer_count,
                                      void* displayed_pixel_buffer,
                                      void* protected_pixel_buffer,
                                      int width,
                                      int height,
                                      int max_track_slots);
    void mark_headless_output_displayed(void* pixel_buffer);
    void protect_headless_output(void* pixel_buffer);
    void release_headless_output(void* pixel_buffer);
    void clear_headless_output();

    bool request_frame_refresh(const char* reason);
    bool request_interaction_frame();
    bool update_presentation_sdr_white_level(double nits);
    bool commit_paused_preview_frame(int timeout_ms,
                                     PresentationBackendFrameInfo* out,
                                     std::string* error);
    bool capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height);
    bool capture_front_buffer_region(int x,
                                     int y,
                                     int width,
                                     int height,
                                     std::vector<uint8_t>& bgra,
                                     int& region_width,
                                     int& region_height);

    bool has_event_callback_for_test() const;
    void enter_terminal_render_loop_error_for_test(const char* reason);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vr
