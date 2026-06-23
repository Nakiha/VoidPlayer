#include "renderer/renderer_internal.h"

#include <utility>

namespace vr {

Renderer::Impl::Impl()
    : timeline_(kPausedHevcSeekSettleDelay)
    , analysis_overlay_renderer_(std::make_unique<AnalysisOverlayRenderer>()) {
}

Renderer::Impl::Impl(PlaybackController& playback)
    : timeline_(playback, kPausedHevcSeekSettleDelay)
    , analysis_overlay_renderer_(std::make_unique<AnalysisOverlayRenderer>()) {
}

Renderer::Impl::~Impl() {
    shutdown();
}

Renderer::Renderer()
    : impl_(std::make_unique<Impl>()) {}

Renderer::Renderer(PlaybackController& playback)
    : impl_(std::make_unique<Impl>(playback)) {}

Renderer::~Renderer() = default;

bool Renderer::initialize(const RendererConfig& config) {
    return impl_->initialize(config);
}

void Renderer::shutdown() {
    impl_->shutdown();
}

void Renderer::play() {
    impl_->play();
}

void Renderer::pause() {
    impl_->pause();
}

void Renderer::seek(int64_t target_pts_us, SeekType type, int64_t request_id) {
    impl_->seek(target_pts_us, type, request_id);
}

void Renderer::set_speed(double speed) {
    impl_->set_speed(speed);
}

void Renderer::set_loop_range(bool enabled, int64_t start_us, int64_t end_us) {
    impl_->set_loop_range(enabled, start_us, end_us);
}

void Renderer::set_audible_track(int file_id) {
    impl_->set_audible_track(file_id);
}

int Renderer::audible_track() const {
    return impl_->audible_track();
}

bool Renderer::has_audio() const {
    return impl_->has_audio();
}

int Renderer::audio_sample_rate() const {
    return impl_->audio_sample_rate();
}

int Renderer::audio_channels() const {
    return impl_->audio_channels();
}

AudioOutputStats Renderer::audio_output_stats() const {
    return impl_->audio_output_stats();
}

void Renderer::step_forward() {
    impl_->step_forward();
}

void Renderer::step_backward() {
    impl_->step_backward();
}

bool Renderer::is_playing() const {
    return impl_->is_playing();
}

bool Renderer::is_initialized() const {
    return impl_->is_initialized();
}

int64_t Renderer::current_pts_us() const {
    return impl_->current_pts_us();
}

double Renderer::current_speed() const {
    return impl_->current_speed();
}

size_t Renderer::track_count() const {
    return impl_->track_count();
}

int64_t Renderer::duration_us() const {
    return impl_->duration_us();
}

int Renderer::add_track(const std::string& video_path, bool use_hardware_decode) {
    return impl_->add_track(video_path, use_hardware_decode);
}

int Renderer::add_track_with_file_id(const std::string& video_path,
                                     int file_id,
                                     bool use_hardware_decode) {
    return impl_->add_track_with_file_id(video_path, file_id, use_hardware_decode);
}

void Renderer::remove_track(int file_id) {
    impl_->remove_track(file_id);
}

bool Renderer::has_track(int slot) const {
    return impl_->has_track(slot);
}

std::pair<int, int> Renderer::track_dimensions(int slot) const {
    return impl_->track_dimensions(slot);
}

std::vector<TrackInfo> Renderer::track_infos() const {
    return impl_->track_infos();
}

std::vector<TrackPerfStats> Renderer::track_perf_stats() const {
    return impl_->track_perf_stats();
}

RendererPresentedAnchorDiagnostics Renderer::presented_anchor_diagnostics() const {
    return impl_->presented_anchor_diagnostics();
}

PresentationBackendMetrics Renderer::presentation_backend_metrics() const {
    return impl_->presentation_backend_metrics();
}

D3D11BackendMetrics Renderer::d3d_backend_metrics() const {
    return impl_->d3d_backend_metrics();
}

PresentationBackendStats Renderer::presentation_backend_stats() const {
    return impl_->presentation_backend_stats();
}

PresentationBackendDiagnostics Renderer::presentation_backend_diagnostics() const {
    return impl_->presentation_backend_diagnostics();
}

std::string Renderer::presentation_backend_last_error() const {
    return impl_->presentation_backend_last_error();
}

bool Renderer::copy_last_presentation_frame_info(PresentationBackendFrameInfo* out) const {
    return impl_->copy_last_presentation_frame_info(out);
}

RendererGpuMemoryStats Renderer::gpu_memory_stats() const {
    return impl_->gpu_memory_stats();
}

bool Renderer::d3d_device_lost() const {
    return impl_->d3d_device_lost();
}

long Renderer::d3d_device_removed_reason() const {
    return impl_->d3d_device_removed_reason();
}

bool Renderer::recover_presentation_device_loss(
    const char* reason,
    long removed_reason) {
    return impl_->recover_presentation_device_loss(reason, removed_reason);
}

RendererDeviceState Renderer::device_state() const {
    return impl_->device_state();
}

void Renderer::set_track_offset(int file_id, int64_t offset_us) {
    impl_->set_track_offset(file_id, offset_us);
}

int64_t Renderer::track_offset_us(int file_id) const {
    return impl_->track_offset_us(file_id);
}

void Renderer::apply_layout(const LayoutState& state) {
    impl_->apply_layout(state);
}

void Renderer::set_background_color(float r, float g, float b, float a) {
    impl_->set_background_color(r, g, b, a);
}

LayoutState Renderer::layout() const {
    return impl_->layout();
}

void Renderer::set_frame_callback(RendererFrameCallback cb) {
    impl_->set_frame_callback(std::move(cb));
}

void Renderer::set_frame_failure_callback(std::function<void(const char*)> cb) {
    impl_->set_frame_failure_callback(std::move(cb));
}

void Renderer::set_event_callback(RendererEventCallback cb) {
    impl_->set_event_callback(std::move(cb));
}

int Renderer::texture_width() const {
    return impl_->texture_width();
}

int Renderer::texture_height() const {
    return impl_->texture_height();
}

bool Renderer::acquire_shared_texture(SharedTextureSnapshot& snapshot) const {
    return impl_->acquire_shared_texture(snapshot);
}

void Renderer::release_shared_texture(int buffer_index, uint64_t buffer_generation) const {
    impl_->release_shared_texture(buffer_index, buffer_generation);
}

bool Renderer::acquire_shared_fp16_texture(
    SharedFp16TextureSnapshot& snapshot) const {
    return impl_->acquire_shared_fp16_texture(snapshot);
}

void Renderer::release_shared_fp16_texture(
    int buffer_index, uint64_t ring_generation) const {
    impl_->release_shared_fp16_texture(buffer_index, ring_generation);
}

void Renderer::set_shared_fp16_frame_callback(std::function<void()> cb) {
    impl_->set_shared_fp16_frame_callback(std::move(cb));
}

bool Renderer::configure_source_cache(
    const std::vector<SourceCacheTrackDescriptor>& descriptors) {
    return impl_->configure_source_cache(descriptors);
}

void Renderer::clear_source_cache(const char* reason) {
    impl_->clear_source_cache(reason);
}

bool Renderer::acquire_source_cache_bundle(
    SharedSourceCacheBundleSnapshot& snapshot) const {
    return impl_->acquire_source_cache_bundle(snapshot);
}

void Renderer::release_source_cache_bundle(
    int buffer_index, uint64_t ring_generation) const {
    impl_->release_source_cache_bundle(buffer_index, ring_generation);
}

void Renderer::set_source_cache_frame_callback(std::function<void()> cb) {
    impl_->set_source_cache_frame_callback(std::move(cb));
}

bool Renderer::prewarm_presentation_target(int width, int height) {
    return impl_->prewarm_presentation_target(width, height);
}

void Renderer::resize(int width, int height) {
    impl_->resize(width, height);
}

bool Renderer::update_headless_output(void* output,
                                      int width,
                                      int height,
                                      int max_track_slots) {
    return impl_->update_headless_output(output, width, height, max_track_slots);
}

bool Renderer::install_headless_output(void* output,
                                       int width,
                                       int height,
                                       int max_track_slots) {
    return impl_->install_headless_output(output, width, height, max_track_slots);
}

bool Renderer::install_headless_output_ring(const void* const* pixel_buffers,
                                            size_t pixel_buffer_count,
                                            void* displayed_pixel_buffer,
                                            void* protected_pixel_buffer,
                                            int width,
                                            int height,
                                            int max_track_slots) {
    return impl_->install_headless_output_ring(pixel_buffers,
                                               pixel_buffer_count,
                                               displayed_pixel_buffer,
                                               protected_pixel_buffer,
                                               width,
                                               height,
                                               max_track_slots);
}

void Renderer::mark_headless_output_displayed(void* pixel_buffer) {
    impl_->mark_headless_output_displayed(pixel_buffer);
}

void Renderer::protect_headless_output(void* pixel_buffer) {
    impl_->protect_headless_output(pixel_buffer);
}

void Renderer::release_headless_output(void* pixel_buffer) {
    impl_->release_headless_output(pixel_buffer);
}

void Renderer::clear_headless_output() {
    impl_->clear_headless_output();
}

bool Renderer::request_frame_refresh(const char* reason) {
    return impl_->request_frame_refresh(reason);
}

bool Renderer::update_presentation_sdr_white_level(double nits) {
    return impl_->update_presentation_sdr_white_level(nits);
}

bool Renderer::draw_current_frame_sources(PresentationBackend& backend,
                                          PresentationSourceFrameTarget* targets,
                                          size_t target_count,
                                          std::string* error) {
    return impl_->draw_current_frame_sources(backend, targets, target_count, error);
}

std::shared_ptr<const AnalysisOverlayPrimitivePackage> Renderer::current_overlay_primitives(
    std::string* error) {
    return impl_->current_overlay_primitives(error);
}

bool Renderer::capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height) {
    return impl_->capture_front_buffer(bgra, width, height);
}

bool Renderer::capture_front_buffer_region(int x,
                                           int y,
                                           int width,
                                           int height,
                                           std::vector<uint8_t>& bgra,
                                           int& region_width,
                                           int& region_height) {
    return impl_->capture_front_buffer_region(
        x, y, width, height, bgra, region_width, region_height);
}

bool Renderer::has_event_callback_for_test() const {
    return impl_->has_event_callback_for_test();
}

void Renderer::enter_terminal_render_loop_error_for_test(const char* reason) {
    impl_->enter_terminal_render_loop_error_for_test(reason);
}

int Renderer::Impl::texture_width() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return surface_state_.width();
}

int Renderer::Impl::texture_height() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return surface_state_.height();
}

} // namespace vr
