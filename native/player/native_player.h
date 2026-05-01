#pragma once

#include "playback/playback_controller.h"
#include "video_renderer/renderer.h"
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace vr {

/// Native player facade that owns playback control and the video renderer as
/// peers. FFI can move to this type in the next phase without changing the
/// renderer/video internals again.
class NativePlayer {
public:
    NativePlayer();
    ~NativePlayer();

    NativePlayer(const NativePlayer&) = delete;
    NativePlayer& operator=(const NativePlayer&) = delete;

    bool initialize(const RendererConfig& config);
    void shutdown();

    void play() { renderer_.play(); }
    void pause() { renderer_.pause(); }
    void seek(int64_t target_pts_us, SeekType type = SeekType::Keyframe) {
        renderer_.seek(target_pts_us, type);
    }
    void set_speed(double speed) { renderer_.set_speed(speed); }
    void set_loop_range(bool enabled, int64_t start_us, int64_t end_us) {
        renderer_.set_loop_range(enabled, start_us, end_us);
    }
    void set_audible_track(int file_id) { renderer_.set_audible_track(file_id); }
    int audible_track() const { return renderer_.audible_track(); }

    void step_forward() { renderer_.step_forward(); }
    void step_backward() { renderer_.step_backward(); }

    bool is_playing() const { return renderer_.is_playing(); }
    bool is_initialized() const { return renderer_.is_initialized(); }
    int64_t current_pts_us() const { return renderer_.current_pts_us(); }
    double current_speed() const { return renderer_.current_speed(); }
    size_t track_count() const { return renderer_.track_count(); }
    int64_t duration_us() const { return renderer_.duration_us(); }

    int add_track(const std::string& video_path) { return renderer_.add_track(video_path); }
    void remove_track(int file_id) { renderer_.remove_track(file_id); }
    bool has_track(int slot) const { return renderer_.has_track(slot); }
    std::pair<int, int> track_dimensions(int slot) const {
        return renderer_.track_dimensions(slot);
    }
    std::vector<TrackInfo> track_infos() const { return renderer_.track_infos(); }
    std::vector<TrackPerfStats> track_perf_stats() const {
        return renderer_.track_perf_stats();
    }
    void set_track_offset(int file_id, int64_t offset_us) {
        renderer_.set_track_offset(file_id, offset_us);
    }

    void apply_layout(const LayoutState& state) { renderer_.apply_layout(state); }
    void set_background_color(float r, float g, float b, float a) {
        renderer_.set_background_color(r, g, b, a);
    }
    LayoutState layout() const { return renderer_.layout(); }

    void set_frame_callback(std::function<void()> cb) {
        renderer_.set_frame_callback(std::move(cb));
    }
    ID3D11Texture2D* shared_texture() const { return renderer_.shared_texture(); }
    int texture_width() const { return renderer_.texture_width(); }
    int texture_height() const { return renderer_.texture_height(); }
    HANDLE shared_texture_handle() const { return renderer_.shared_texture_handle(); }
    bool acquire_shared_texture(SharedTextureSnapshot& snapshot) const {
        return renderer_.acquire_shared_texture(snapshot);
    }
    std::mutex& texture_mutex() const { return renderer_.texture_mutex(); }
    void resize(int width, int height) { renderer_.resize(width, height); }
    bool capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height) {
        return renderer_.capture_front_buffer(bgra, width, height);
    }

    PlaybackController& playback() { return playback_; }
    const PlaybackController& playback() const { return playback_; }
    Renderer& renderer() { return renderer_; }
    const Renderer& renderer() const { return renderer_; }

private:
    PlaybackController playback_;
    Renderer renderer_;
};

} // namespace vr
