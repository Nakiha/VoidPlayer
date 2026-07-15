#include "windows/player/native_player.h"

#include <mutex>
#include <utility>

namespace vr {

WindowsNativePlayer::WindowsNativePlayer()
    : renderer_(std::make_unique<Renderer>()) {}

WindowsNativePlayer::~WindowsNativePlayer() {
  shutdown();
}

bool WindowsNativePlayer::initialize(const RendererConfig& config) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (ready_locked()) {
    return false;
  }
  if (!renderer_) {
    renderer_ = std::make_unique<Renderer>();
  }
  return renderer_->initialize(config);
}

void WindowsNativePlayer::shutdown() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (renderer_) {
    renderer_->set_frame_callback({});
    renderer_->set_frame_failure_callback({});
    renderer_->set_event_callback({});
    renderer_->shutdown();
  }
}

bool WindowsNativePlayer::ready_locked() const {
  return renderer_ && renderer_->is_initialized();
}

bool WindowsNativePlayer::initialized() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked();
}

#define VP_WINDOWS_PLAYER_VOID(method, ...)             \
  do {                                                   \
    std::shared_lock<std::shared_mutex> lock(mutex_);    \
    if (ready_locked()) {                                \
      renderer_->method(__VA_ARGS__);                    \
    }                                                    \
  } while (false)

void WindowsNativePlayer::play() { VP_WINDOWS_PLAYER_VOID(play); }
void WindowsNativePlayer::pause() { VP_WINDOWS_PLAYER_VOID(pause); }
void WindowsNativePlayer::seek(int64_t pts_us, int64_t request_id) {
  VP_WINDOWS_PLAYER_VOID(seek, pts_us, SeekType::Keyframe, request_id);
}
void WindowsNativePlayer::step_forward() {
  VP_WINDOWS_PLAYER_VOID(step_forward);
}
void WindowsNativePlayer::step_backward() {
  VP_WINDOWS_PLAYER_VOID(step_backward);
}
void WindowsNativePlayer::set_speed(double speed) {
  VP_WINDOWS_PLAYER_VOID(set_speed, speed);
}
void WindowsNativePlayer::set_loop_range(bool enabled,
                                         int64_t start_us,
                                         int64_t end_us) {
  VP_WINDOWS_PLAYER_VOID(set_loop_range, enabled, start_us, end_us);
}
void WindowsNativePlayer::set_audible_track(int file_id) {
  VP_WINDOWS_PLAYER_VOID(set_audible_track, file_id);
}
void WindowsNativePlayer::set_track_offset(int file_id, int64_t offset_us) {
  VP_WINDOWS_PLAYER_VOID(set_track_offset, file_id, offset_us);
}

int64_t WindowsNativePlayer::track_offset_us(int file_id) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->track_offset_us(file_id) : 0;
}

void WindowsNativePlayer::set_background_color(float red,
                                               float green,
                                               float blue,
                                               float alpha) {
  VP_WINDOWS_PLAYER_VOID(set_background_color, red, green, blue, alpha);
}
void WindowsNativePlayer::apply_layout(const LayoutState& layout) {
  VP_WINDOWS_PLAYER_VOID(apply_layout, layout);
}
void WindowsNativePlayer::apply_interaction_layout(const LayoutState& layout) {
  VP_WINDOWS_PLAYER_VOID(apply_interaction_layout, layout);
}
void WindowsNativePlayer::resize(int width, int height) {
  VP_WINDOWS_PLAYER_VOID(resize, width, height);
}

#undef VP_WINDOWS_PLAYER_VOID

int WindowsNativePlayer::add_track(const std::string& path,
                                   bool use_hardware_decode) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->add_track(path, use_hardware_decode) : -1;
}

void WindowsNativePlayer::remove_track(int file_id) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (ready_locked()) {
    renderer_->remove_track(file_id);
  }
}

bool WindowsNativePlayer::is_playing() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() && renderer_->is_playing();
}

int64_t WindowsNativePlayer::current_pts_us() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->current_pts_us() : 0;
}

int64_t WindowsNativePlayer::duration_us() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->duration_us() : 0;
}

LayoutState WindowsNativePlayer::layout() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->layout() : LayoutState{};
}

std::vector<TrackInfo> WindowsNativePlayer::tracks() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->track_infos() : std::vector<TrackInfo>{};
}

std::vector<TrackPerfStats> WindowsNativePlayer::track_perf_stats() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->track_perf_stats()
                        : std::vector<TrackPerfStats>{};
}

RendererGpuMemoryStats WindowsNativePlayer::gpu_memory_stats() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->gpu_memory_stats()
                        : RendererGpuMemoryStats{};
}

PresentationBackendMetrics WindowsNativePlayer::presentation_metrics() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->presentation_backend_metrics()
                        : PresentationBackendMetrics{};
}

PresentationBackendStats WindowsNativePlayer::presentation_stats() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->presentation_backend_stats()
                        : PresentationBackendStats{};
}

PresentationBackendDiagnostics
WindowsNativePlayer::presentation_diagnostics() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->presentation_backend_diagnostics()
                        : PresentationBackendDiagnostics{};
}

std::string WindowsNativePlayer::presentation_error() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() ? renderer_->presentation_backend_last_error()
                        : std::string("player-unavailable");
}

bool WindowsNativePlayer::copy_last_frame_info(
    PresentationBackendFrameInfo* out) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() && renderer_->copy_last_presentation_frame_info(out);
}

void WindowsNativePlayer::set_frame_callback(RendererFrameCallback callback) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (renderer_) {
    renderer_->set_frame_callback(std::move(callback));
  }
}

void WindowsNativePlayer::set_frame_failure_callback(
    std::function<void(const char*)> callback) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (renderer_) {
    renderer_->set_frame_failure_callback(std::move(callback));
  }
}

void WindowsNativePlayer::set_event_callback(RendererEventCallback callback) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (renderer_) {
    renderer_->set_event_callback(std::move(callback));
  }
}

void WindowsNativePlayer::mark_target_displayed(void* texture) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (ready_locked()) {
    renderer_->mark_offscreen_target_displayed(texture);
  }
}

void WindowsNativePlayer::protect_target(void* texture) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (ready_locked()) {
    renderer_->protect_offscreen_target(texture);
  }
}

void WindowsNativePlayer::release_target(void* texture) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (ready_locked()) {
    renderer_->release_offscreen_target(texture);
  }
}

bool WindowsNativePlayer::install_target_ring(
    const void* const* textures,
    size_t texture_count,
    void* displayed_texture,
    void* protected_texture,
    int width,
    int height,
    int max_track_slots) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() && renderer_->install_offscreen_target_ring(
                               textures, texture_count, displayed_texture,
                               protected_texture, width, height,
                               max_track_slots);
}

bool WindowsNativePlayer::update_presentation_sdr_white_level(double nits) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() && renderer_->update_presentation_sdr_white_level(nits);
}

bool WindowsNativePlayer::request_frame_refresh(const char* reason) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() && renderer_->request_frame_refresh(reason);
}

bool WindowsNativePlayer::request_interaction_frame() {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() && renderer_->request_interaction_frame();
}

bool WindowsNativePlayer::capture_front_buffer(std::vector<uint8_t>& bgra,
                                               int& width,
                                               int& height) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() && renderer_->capture_front_buffer(bgra, width, height);
}

bool WindowsNativePlayer::capture_front_buffer_region(
    int x,
    int y,
    int width,
    int height,
    std::vector<uint8_t>& bgra,
    int& region_width,
    int& region_height) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return ready_locked() && renderer_->capture_front_buffer_region(
                               x, y, width, height, bgra,
                               region_width, region_height);
}

}  // namespace vr
