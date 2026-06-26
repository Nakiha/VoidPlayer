#include "renderer/track/renderer_track_controller.h"

#include "renderer/track/renderer_track_mutation_controller.h"
#include "renderer/track/renderer_track_presentation_model.h"
#include "renderer/track/renderer_track_registry.h"

#include <memory>

namespace vr {

RendererTrackController::RendererTrackController()
    : registry_(std::make_unique<RendererTrackRegistry>()),
      mutation_(std::make_unique<RendererTrackMutationController>(*registry_)),
      presentation_model_(
          std::make_unique<RendererTrackPresentationModel>(*registry_)) {}

RendererTrackController::~RendererTrackController() = default;

std::unique_ptr<TrackPipeline> RendererTrackController::create_pipeline(
    const std::string& path,
    bool hw_decode,
    RenderBackendKind render_backend,
    void* render_device,
    std::recursive_mutex* device_mutex,
    const SeekRequest* initial_seek) const {
    return registry_->create_pipeline(
        path, hw_decode, render_backend, render_device, device_mutex, initial_seek);
}

bool RendererTrackController::configure_and_start_pipeline(
    TrackPipeline& pipeline,
    const TrackPipelineStartConfig& config,
    const TrackPipelineStartHooks& hooks,
    const char* log_context) const {
    return mutation_->configure_and_start_pipeline(
        pipeline, config, hooks, log_context);
}

void RendererTrackController::stop_detached_pipeline(
    size_t slot,
    std::unique_ptr<TrackPipeline>& track) const {
    mutation_->stop_detached_pipeline(slot, track);
}

void RendererTrackController::assign_missing_generations() {
    registry_->assign_missing_generations();
}

int RendererTrackController::first_active_slot() const {
    return registry_->first_active_slot();
}

int RendererTrackController::find_empty_slot() const {
    return registry_->find_empty_slot();
}

int RendererTrackController::find_slot_by_file_id(int file_id) const {
    return registry_->find_slot_by_file_id(file_id);
}

int RendererTrackController::audio_info_slot(int preferred_file_id) const {
    return registry_->audio_info_slot(preferred_file_id);
}

int RendererTrackController::audio_sample_rate_for_slot(int slot) const {
    return registry_->audio_sample_rate_for_slot(slot);
}

int RendererTrackController::audio_channels_for_slot(int slot) const {
    return registry_->audio_channels_for_slot(slot);
}

bool RendererTrackController::uses_hardware_codec(AVCodecID codec_id) const {
    return registry_->uses_hardware_codec(codec_id);
}

int64_t RendererTrackController::min_current_frame_duration_us() const {
    return registry_->min_current_frame_duration_us();
}

bool RendererTrackController::has_slot(int slot) const {
    return registry_->has_slot(slot);
}

std::pair<int, int> RendererTrackController::dimensions_for_slot(int slot) const {
    return registry_->dimensions_for_slot(slot);
}

std::vector<TrackInfo> RendererTrackController::infos() const {
    return presentation_model_->infos();
}

std::vector<TrackPerfStats> RendererTrackController::perf_stats(
    const PresentDecision& last_decision,
    std::chrono::steady_clock::time_point now) {
    return presentation_model_->perf_stats(last_decision, now);
}

TrackGpuMemoryStatsCollectionResult RendererTrackController::gpu_memory_stats(
    const std::array<uint64_t, kMaxTracks>& presenter_copy_texture_bytes_by_slot) const {
    return presentation_model_->gpu_memory_stats(
        presenter_copy_texture_bytes_by_slot);
}

LayoutTrackGeometryList RendererTrackController::layout_track_geometry() const {
    return presentation_model_->layout_track_geometry();
}

void RendererTrackController::populate_draw_tracks(
    RendererDrawTrackSnapshotList& out) const {
    presentation_model_->populate_draw_tracks(out);
}

std::vector<RendererLayoutTrackReference>
RendererTrackController::layout_track_references() const {
    return presentation_model_->layout_track_references();
}

bool RendererTrackController::has_active_tracks() const {
    return registry_->has_active_tracks();
}

size_t RendererTrackController::count() const {
    return registry_->count();
}

bool RendererTrackController::has_preroll_blocking_track() const {
    return registry_->has_preroll_blocking_track();
}

bool RendererTrackController::has_buffering_track() const {
    return registry_->has_buffering_track();
}

std::vector<RenderLoopTrackDiagnosticSnapshot>
RendererTrackController::render_loop_diagnostics() const {
    return presentation_model_->render_loop_diagnostics();
}

void RendererTrackController::set_video_decode_paused(
    bool paused,
    const std::function<void(size_t slot, TrackPipeline& track, bool paused)>&
        set_decode_paused) {
    mutation_->set_video_decode_paused(paused, set_decode_paused);
}

void RendererTrackController::set_decode_paused_for_all(
    bool paused,
    const TrackDecodePauseHooks& hooks) {
    mutation_->set_decode_paused_for_all(paused, hooks);
}

void RendererTrackController::apply_playback_decode_state(
    bool playback_active,
    const TrackPlaybackDecodeStateHooks& hooks) {
    mutation_->apply_playback_decode_state(playback_active, hooks);
}

bool RendererTrackController::apply_track_offset(
    int file_id,
    int64_t offset_us,
    const TrackOffsetMutationHooks& hooks) {
    return mutation_->apply_track_offset(file_id, offset_us, hooks);
}

StepDecisionBuildResult RendererTrackController::build_step_forward_decision(
    int64_t current_pts_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    return presentation_model_->build_step_forward_decision(
        current_pts_us, last_decision, decision);
}

StepDecisionApplication RendererTrackController::apply_step_forward_decision(
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision) {
    return mutation_->apply_step_forward_decision(
        current_pts_us, decision, last_decision);
}

void RendererTrackController::discard_step_forward_consumed_frames(
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision) {
    mutation_->discard_step_forward_consumed_frames(
        current_pts_us, decision, last_decision);
}

StepForwardExactSeekTarget
RendererTrackController::choose_step_forward_exact_seek_target(
    int64_t clock_pts_us,
    const PresentDecision& last_decision) const {
    return presentation_model_->choose_step_forward_exact_seek_target(
        clock_pts_us, last_decision);
}

bool RendererTrackController::build_step_backward_decision(
    int64_t current_pts_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    return presentation_model_->build_step_backward_decision(
        current_pts_us, last_decision, decision);
}

StepDecisionApplication RendererTrackController::apply_step_backward_decision(
    const PresentDecision& decision) {
    return mutation_->apply_step_backward_decision(decision);
}

StepBackwardExactSeekTarget
RendererTrackController::choose_step_backward_exact_seek_target(
    int64_t clock_pts_us,
    const PresentDecision& last_decision) const {
    return presentation_model_->choose_step_backward_exact_seek_target(
        clock_pts_us, last_decision);
}

void RendererTrackController::apply_carry_forward(
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    presentation_model_->apply_carry_forward(last_decision, decision);
}

void RendererTrackController::filter_present_decision(PresentDecision& decision) const {
    presentation_model_->filter_present_decision(decision);
}

std::vector<SeekPreviewPresentedTrackEvent>
RendererTrackController::collect_seek_preview_presented_events(
    const PresentDecision& decision,
    int64_t request_id,
    int64_t target_pts_us) const {
    return presentation_model_->collect_seek_preview_presented_events(
        decision, request_id, target_pts_us);
}

std::vector<LayoutTrackGeometryUpdate>
RendererTrackController::update_layout_track_geometry_from_decision(
    const PresentDecision& decision) {
    return presentation_model_->update_layout_track_geometry_from_decision(
        decision);
}

EmptyBufferEofClamp RendererTrackController::empty_buffer_eof_clamp(
    const PresentDecision& last_decision) const {
    return presentation_model_->empty_buffer_eof_clamp(last_decision);
}

std::optional<int64_t> RendererTrackController::next_frame_event_pts_us(
    int64_t current_pts_us) const {
    return presentation_model_->next_frame_event_pts_us(current_pts_us);
}

RendererPausedCachedDecision RendererTrackController::paused_cached_decision(
    const PresentDecision& last_decision) const {
    return presentation_model_->paused_cached_decision(last_decision);
}

RendererPausedLayoutDecision RendererTrackController::paused_layout_decision(
    const PresentDecision& last_decision) const {
    return presentation_model_->paused_layout_decision(last_decision);
}

RendererPausedRefreshDecision RendererTrackController::paused_refresh_decision(
    const PresentDecision& last_decision,
    const std::optional<PresentDecision>& evaluated_decision,
    bool decoded_preview_refresh) const {
    return presentation_model_->paused_refresh_decision(
        last_decision, evaluated_decision, decoded_preview_refresh);
}

bool RendererTrackController::has_complete_cached_decision(
    const PresentDecision& last_decision) const {
    return presentation_model_->has_complete_cached_decision(last_decision);
}

PausedPreviewSnapshot RendererTrackController::paused_preview_snapshot() const {
    return presentation_model_->paused_preview_snapshot();
}

RendererTrackReferenceSnapshot
RendererTrackController::first_active_reference() const {
    return presentation_model_->first_active_reference();
}

int RendererTrackController::next_file_id() const {
    return registry_->next_file_id_value();
}

void RendererTrackController::set_next_file_id(int value) {
    registry_->set_next_file_id(value);
}

int RendererTrackController::allocate_next_file_id() {
    return registry_->allocate_next_file_id();
}

uint64_t RendererTrackController::allocate_generation() {
    return registry_->allocate_generation();
}

void RendererTrackController::reset_ids() {
    registry_->reset_ids();
}

InitialTrackOpenResult RendererTrackController::open_initial_tracks(
    const std::vector<std::string>& video_paths,
    bool use_hardware_decode,
    const InitialTrackOpenHooks& hooks,
    const char* log_context) {
    return registry_->open_initial_tracks(
        video_paths, use_hardware_decode, hooks, log_context);
}

void RendererTrackController::bind_to_render_sink(RenderSink& render_sink) const {
    registry_->bind_to_render_sink(render_sink);
}

void RendererTrackController::stop_all(
    const RendererTrackBeforeStopCallback& before_stop) {
    registry_->stop_all(before_stop);
}

RendererTrackAddReservation RendererTrackController::reserve_add_track(
    int requested_file_id) {
    return registry_->reserve_add_track(requested_file_id);
}

bool RendererTrackController::can_commit_add(size_t slot) const {
    return mutation_->can_commit_add(slot);
}

TrackPipeline* RendererTrackController::commit_new_track(
    size_t slot,
    std::unique_ptr<TrackPipeline> pipeline,
    const TrackAddCommitHooks& hooks) {
    return mutation_->commit_new_track(slot, std::move(pipeline), hooks);
}

RendererTrackDetachResult RendererTrackController::detach_and_compact_by_file_id(
    int file_id,
    const RendererTrackDetachHooks& hooks) {
    return mutation_->detach_and_compact_by_file_id(file_id, hooks);
}

RendererTrackRecreateDetachResult RendererTrackController::detach_for_recreate(
    size_t slot,
    const RendererTrackRecreateDetachHooks& hooks) {
    return mutation_->detach_for_recreate(slot, hooks);
}

bool RendererTrackController::can_commit_recreated_track(size_t slot) const {
    return mutation_->can_commit_recreated_track(slot);
}

TrackPipeline* RendererTrackController::commit_recreated_track(
    size_t slot,
    std::unique_ptr<TrackPipeline> pipeline,
    const TrackAddCommitHooks& hooks) {
    return mutation_->commit_recreated_track(slot, std::move(pipeline), hooks);
}

std::vector<RendererTrackSeekApplicationResult>
RendererTrackController::apply_seek_to_all(
    int64_t target_pts_us,
    SeekType type,
    bool playing,
    bool force_recreate_paused_hevc,
    const RendererTrackSeekHooks& hooks) {
    return mutation_->apply_seek_to_all(
        target_pts_us, type, playing, force_recreate_paused_hevc, hooks);
}

bool RendererTrackController::apply_seek_to_all_and_log(
    int64_t target_pts_us,
    SeekType type,
    bool playing,
    bool force_recreate_paused_hevc,
    const RendererTrackSeekHooks& hooks) {
    return mutation_->apply_seek_to_all_and_log(
        target_pts_us, type, playing, force_recreate_paused_hevc, hooks);
}

int64_t RendererTrackController::cached_duration_us() const {
    return registry_->cached_duration();
}

void RendererTrackController::set_cached_duration_us(int64_t duration_us) {
    registry_->set_cached_duration(duration_us);
}

void RendererTrackController::extend_cached_duration_with(
    const TrackPipeline& track) {
    registry_->extend_cached_duration_with(track);
}

void RendererTrackController::recompute_cached_duration() {
    registry_->recompute_cached_duration();
}

int64_t RendererTrackController::effective_duration_us() const {
    return registry_->effective_duration_us();
}

int64_t RendererTrackController::offset_us_for_file_id(int file_id) const {
    return registry_->offset_us_for_file_id(file_id);
}

TrackAddSeekResult RendererTrackController::prepare_add_track_seek_to_clock(
    TrackPipeline& track,
    int64_t current_pts_us,
    bool was_playing,
    const TrackAddSeekHooks& hooks) const {
    return registry_->prepare_add_track_seek_to_clock(
        track, current_pts_us, was_playing, hooks);
}

void RendererTrackController::reset_perf_baseline(
    std::chrono::steady_clock::time_point now) {
    presentation_model_->reset_perf_baseline(now);
}

void RendererTrackController::reset_perf_baseline() {
    presentation_model_->reset_perf_baseline();
}

} // namespace vr
