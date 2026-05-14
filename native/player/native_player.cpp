#include "player/native_player.h"
#include "audio/audio_output_factory.h"
#include "video_renderer/renderer_config_validation.h"

namespace vr {

NativePlayer::NativePlayer()
    : playback_(create_default_audio_output)
    , renderer_(playback_) {}

NativePlayer::~NativePlayer() {
    shutdown();
}

bool NativePlayer::initialize(const RendererConfig& config) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (state_ != State::Created || renderer_.is_initialized()) {
        return false;
    }

    if (!validate_renderer_config(config)) {
        return false;
    }

    state_ = State::Initializing;
    playback_.start_session();
    if (!renderer_.initialize(config)) {
        playback_.stop_session();
        state_ = State::Created;
        return false;
    }
    state_ = State::Initialized;
    return true;
}

void NativePlayer::shutdown() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (state_ == State::ShuttingDown) {
        return;
    }
    state_ = State::ShuttingDown;
    renderer_.shutdown();
    playback_.stop_session();
    state_ = State::Created;
}

bool NativePlayer::renderer_ready_locked() const {
    return state_ == State::Initialized && renderer_.is_initialized();
}

void NativePlayer::play() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.play();
}

void NativePlayer::pause() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.pause();
}

void NativePlayer::seek(int64_t target_pts_us,
                        SeekType type,
                        int64_t request_id) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.seek(target_pts_us, type, request_id);
}

void NativePlayer::set_speed(double speed) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.set_speed(speed);
}

void NativePlayer::set_loop_range(bool enabled, int64_t start_us, int64_t end_us) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.set_loop_range(enabled, start_us, end_us);
}

void NativePlayer::set_audible_track(int file_id) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.set_audible_track(file_id);
}

int NativePlayer::audible_track() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return -1;
    }
    return renderer_.audible_track();
}

void NativePlayer::step_forward() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.step_forward();
}

void NativePlayer::step_backward() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.step_backward();
}

bool NativePlayer::is_playing() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() && renderer_.is_playing();
}

bool NativePlayer::is_initialized() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked();
}

int64_t NativePlayer::current_pts_us() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.current_pts_us();
}

double NativePlayer::current_speed() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 1.0;
    }
    return renderer_.current_speed();
}

size_t NativePlayer::track_count() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.track_count();
}

int64_t NativePlayer::duration_us() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.duration_us();
}

int NativePlayer::add_track(const std::string& video_path, bool use_hardware_decode) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return -1;
    }
    return renderer_.add_track(video_path, use_hardware_decode);
}

void NativePlayer::remove_track(int file_id) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.remove_track(file_id);
}

bool NativePlayer::has_track(int slot) const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() && renderer_.has_track(slot);
}

std::pair<int, int> NativePlayer::track_dimensions(int slot) const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {0, 0};
    }
    return renderer_.track_dimensions(slot);
}

std::vector<TrackInfo> NativePlayer::track_infos() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {};
    }
    return renderer_.track_infos();
}

std::vector<TrackPerfStats> NativePlayer::track_perf_stats() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {};
    }
    return renderer_.track_perf_stats();
}

RendererGpuMemoryStats NativePlayer::gpu_memory_stats() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {};
    }
    return renderer_.gpu_memory_stats();
}

bool NativePlayer::d3d_device_lost() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() && renderer_.d3d_device_lost();
}

long NativePlayer::d3d_device_removed_reason() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.d3d_device_removed_reason();
}

void NativePlayer::set_track_offset(int file_id, int64_t offset_us) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.set_track_offset(file_id, offset_us);
}

void NativePlayer::apply_layout(const LayoutState& state) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.apply_layout(state);
}

void NativePlayer::set_background_color(float r, float g, float b, float a) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.set_background_color(r, g, b, a);
}

LayoutState NativePlayer::layout() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {};
    }
    return renderer_.layout();
}

void NativePlayer::set_frame_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    renderer_.set_frame_callback(std::move(cb));
}

void NativePlayer::set_event_callback(RendererEventCallback cb) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    renderer_.set_event_callback(std::move(cb));
}

int NativePlayer::texture_width() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.texture_width();
}

int NativePlayer::texture_height() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.texture_height();
}

bool NativePlayer::acquire_shared_texture(SharedTextureSnapshot& snapshot) const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        snapshot = {};
        return false;
    }
    return renderer_.acquire_shared_texture(snapshot);
}

void NativePlayer::release_shared_texture(int buffer_index,
                                          uint64_t buffer_generation) const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.release_shared_texture(buffer_index, buffer_generation);
}

void NativePlayer::resize(int width, int height) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.resize(width, height);
}

bool NativePlayer::capture_front_buffer(std::vector<uint8_t>& bgra,
                                        int& width,
                                        int& height) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    return renderer_.capture_front_buffer(bgra, width, height);
}

} // namespace vr
