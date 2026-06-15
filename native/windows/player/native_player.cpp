#include "windows/player/native_player.h"
#include "audio/audio_output_factory.h"
#include "renderer/renderer_config_validation.h"
#include <mutex>

namespace vr {

NativePlayer::NativePlayer()
    : playback_(create_default_audio_output)
    , renderer_(playback_) {}

NativePlayer::~NativePlayer() {
    shutdown();
}

bool NativePlayer::initialize(const RendererConfig& config) {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
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
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
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
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.play();
}

void NativePlayer::pause() {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.pause();
}

void NativePlayer::seek(int64_t target_pts_us,
                        SeekType type,
                        int64_t request_id) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.seek(target_pts_us, type, request_id);
}

void NativePlayer::set_speed(double speed) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.set_speed(speed);
}

void NativePlayer::set_loop_range(bool enabled, int64_t start_us, int64_t end_us) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.set_loop_range(enabled, start_us, end_us);
}

void NativePlayer::set_audible_track(int file_id) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.set_audible_track(file_id);
}

int NativePlayer::audible_track() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return -1;
    }
    return renderer_.audible_track();
}

void NativePlayer::step_forward() {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.step_forward();
}

void NativePlayer::step_backward() {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.step_backward();
}

bool NativePlayer::is_playing() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() && renderer_.is_playing();
}

bool NativePlayer::is_initialized() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked();
}

int64_t NativePlayer::current_pts_us() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.current_pts_us();
}

double NativePlayer::current_speed() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 1.0;
    }
    return renderer_.current_speed();
}

size_t NativePlayer::track_count() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.track_count();
}

int64_t NativePlayer::duration_us() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.duration_us();
}

int NativePlayer::add_track(const std::string& video_path, bool use_hardware_decode) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return -1;
    }
    return renderer_.add_track(video_path, use_hardware_decode);
}

void NativePlayer::remove_track(int file_id) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.remove_track(file_id);
}

bool NativePlayer::has_track(int slot) const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() && renderer_.has_track(slot);
}

std::pair<int, int> NativePlayer::track_dimensions(int slot) const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {0, 0};
    }
    return renderer_.track_dimensions(slot);
}

std::vector<TrackInfo> NativePlayer::track_infos() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {};
    }
    return renderer_.track_infos();
}

std::vector<TrackPerfStats> NativePlayer::track_perf_stats() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {};
    }
    return renderer_.track_perf_stats();
}

RendererGpuMemoryStats NativePlayer::gpu_memory_stats() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {};
    }
    return renderer_.gpu_memory_stats();
}

PresentationBackendDiagnostics
NativePlayer::presentation_backend_diagnostics() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {};
    }
    return renderer_.presentation_backend_diagnostics();
}

AudioOutputStats NativePlayer::audio_output_stats() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {};
    }
    return renderer_.audio_output_stats();
}

bool NativePlayer::d3d_device_lost() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() && renderer_.d3d_device_lost();
}

long NativePlayer::d3d_device_removed_reason() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.d3d_device_removed_reason();
}

bool NativePlayer::recover_presentation_device_loss(
    const char* reason,
    long removed_reason) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() &&
           renderer_.recover_presentation_device_loss(reason, removed_reason);
}

void NativePlayer::set_track_offset(int file_id, int64_t offset_us) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.set_track_offset(file_id, offset_us);
}

void NativePlayer::apply_layout(const LayoutState& state) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.apply_layout(state);
}

void NativePlayer::set_background_color(float r, float g, float b, float a) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.set_background_color(r, g, b, a);
}

LayoutState NativePlayer::layout() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return {};
    }
    return renderer_.layout();
}

void NativePlayer::set_frame_callback(std::function<void()> cb) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    renderer_.set_frame_callback(
        [cb = std::move(cb)](const PresentationBackendFrameInfo*) {
            if (cb) {
                cb();
            }
        });
}

void NativePlayer::set_event_callback(RendererEventCallback cb) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    renderer_.set_event_callback(std::move(cb));
}

int NativePlayer::texture_width() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.texture_width();
}

int NativePlayer::texture_height() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return 0;
    }
    return renderer_.texture_height();
}

bool NativePlayer::acquire_shared_texture(SharedTextureSnapshot& snapshot) const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        snapshot = {};
        return false;
    }
    return renderer_.acquire_shared_texture(snapshot);
}

void NativePlayer::release_shared_texture(int buffer_index,
                                          uint64_t buffer_generation) const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.release_shared_texture(buffer_index, buffer_generation);
}

bool NativePlayer::acquire_shared_fp16_texture(
    SharedFp16TextureSnapshot& snapshot) const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() &&
           renderer_.acquire_shared_fp16_texture(snapshot);
}

void NativePlayer::release_shared_fp16_texture(
    int buffer_index, uint64_t ring_generation) const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (renderer_ready_locked()) {
        renderer_.release_shared_fp16_texture(
            buffer_index, ring_generation);
    }
}

void NativePlayer::set_shared_fp16_frame_callback(
    std::function<void()> cb) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (renderer_ready_locked()) {
        renderer_.set_shared_fp16_frame_callback(std::move(cb));
    }
}

bool NativePlayer::configure_source_cache(
    const std::vector<SourceCacheTrackDescriptor>& descriptors) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() &&
           renderer_.configure_source_cache(descriptors);
}

void NativePlayer::clear_source_cache(const char* reason) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (renderer_ready_locked()) {
        renderer_.clear_source_cache(reason);
    }
}

bool NativePlayer::acquire_source_cache_bundle(
    SharedSourceCacheBundleSnapshot& snapshot) const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() &&
           renderer_.acquire_source_cache_bundle(snapshot);
}

void NativePlayer::release_source_cache_bundle(
    int buffer_index, uint64_t ring_generation) const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (renderer_ready_locked()) {
        renderer_.release_source_cache_bundle(
            buffer_index, ring_generation);
    }
}

void NativePlayer::set_source_cache_frame_callback(
    std::function<void()> cb) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (renderer_ready_locked()) {
        renderer_.set_source_cache_frame_callback(std::move(cb));
    }
}

bool NativePlayer::request_frame_refresh(const char* reason) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() &&
           renderer_.request_frame_refresh(reason);
}

bool NativePlayer::update_presentation_sdr_white_level(double nits) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return renderer_ready_locked() &&
           renderer_.update_presentation_sdr_white_level(nits);
}

std::shared_ptr<const AnalysisOverlayPrimitivePackage>
NativePlayer::current_overlay_primitives(std::string* error) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        if (error) {
            *error = "renderer is not active";
        }
        return {};
    }
    return renderer_.current_overlay_primitives(error);
}

void NativePlayer::resize(int width, int height) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        return;
    }
    renderer_.resize(width, height);
}

bool NativePlayer::capture_front_buffer(std::vector<uint8_t>& bgra,
                                        int& width,
                                        int& height) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    return renderer_.capture_front_buffer(bgra, width, height);
}

bool NativePlayer::capture_front_buffer_region(int x,
                                               int y,
                                               int width,
                                               int height,
                                               std::vector<uint8_t>& bgra,
                                               int& region_width,
                                               int& region_height) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (!renderer_ready_locked()) {
        bgra.clear();
        region_width = 0;
        region_height = 0;
        return false;
    }
    return renderer_.capture_front_buffer_region(
        x, y, width, height, bgra, region_width, region_height);
}

} // namespace vr
