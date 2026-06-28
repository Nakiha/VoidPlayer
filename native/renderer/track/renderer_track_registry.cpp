#include "renderer/track/renderer_track_registry.h"

#include "renderer/track/track_lifecycle.h"
#include "renderer/track/track_present_policy.h"
#include "renderer/track/track_preroll_policy.h"
#include "renderer/track/track_step_policy.h"

#include <algorithm>

namespace vr {

std::unique_ptr<TrackPipeline> RendererTrackRegistry::create_pipeline(
    const std::string& path,
    bool hw_decode,
    RenderBackendKind render_backend,
    void* render_device,
    std::recursive_mutex* device_mutex,
    const SeekRequest* initial_seek) const {
    TrackPipelineOpenOptions options;
    options.render_backend = render_backend;
    if (render_backend == RenderBackendKind::WgpuD3D12 && render_device) {
        options.use_default_decode_device_mode = false;
        options.decode_device_mode = DecodeDeviceMode::SharedRenderDevice;
        options.render_device = render_device;
        options.device_mutex = device_mutex;
    }
    return factory_.create_opened_pipeline(
        path, hw_decode, initial_seek, options);
}

TrackPipelineManager& RendererTrackRegistry::mutable_tracks_for_mutation() {
    return tracks_;
}

const TrackPipelineManager& RendererTrackRegistry::tracks_for_snapshot() const {
    return tracks_;
}

void RendererTrackRegistry::assign_missing_generations() {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i] && tracks_[i]->generation == 0) {
            tracks_[i]->generation = allocate_generation();
        }
    }
}

int RendererTrackRegistry::first_active_slot() const {
    return tracks_.first_active_slot();
}

int RendererTrackRegistry::find_empty_slot() const {
    return tracks_.find_empty_slot();
}

int RendererTrackRegistry::find_slot_by_file_id(int file_id) const {
    return tracks_.find_slot_by_file_id(file_id);
}

int RendererTrackRegistry::audio_info_slot(int preferred_file_id) const {
    if (preferred_file_id >= 0) {
        const int slot = find_slot_by_file_id(preferred_file_id);
        if (slot >= 0 && tracks_[static_cast<size_t>(slot)] &&
            tracks_[static_cast<size_t>(slot)]->demux_thread) {
            const auto& stats =
                tracks_[static_cast<size_t>(slot)]->demux_thread->stats();
            if (stats.audio_stream_index >= 0 && stats.audio_codec_params) {
                return slot;
            }
        }
    }

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i] || !tracks_[i]->demux_thread) {
            continue;
        }
        const auto& stats = tracks_[i]->demux_thread->stats();
        if (stats.audio_stream_index >= 0 && stats.audio_codec_params) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int RendererTrackRegistry::audio_sample_rate_for_slot(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks) ||
        !tracks_[static_cast<size_t>(slot)] ||
        !tracks_[static_cast<size_t>(slot)]->demux_thread) {
        return 0;
    }
    return tracks_[static_cast<size_t>(slot)]->demux_thread->stats().sample_rate;
}

int RendererTrackRegistry::audio_channels_for_slot(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks) ||
        !tracks_[static_cast<size_t>(slot)] ||
        !tracks_[static_cast<size_t>(slot)]->demux_thread) {
        return 0;
    }
    return tracks_[static_cast<size_t>(slot)]->demux_thread->stats().channels;
}

bool RendererTrackRegistry::uses_hardware_codec(AVCodecID codec_id) const {
    return any_track_uses_hardware_codec(tracks_, codec_id);
}

int64_t RendererTrackRegistry::min_current_frame_duration_us() const {
    return compute_min_current_frame_duration_us(tracks_);
}

bool RendererTrackRegistry::has_slot(int slot) const {
    return slot >= 0 &&
           slot < static_cast<int>(kMaxTracks) &&
           tracks_[static_cast<size_t>(slot)] != nullptr;
}

std::pair<int, int> RendererTrackRegistry::dimensions_for_slot(int slot) const {
    if (!has_slot(slot)) {
        return {0, 0};
    }
    const auto& track = tracks_[static_cast<size_t>(slot)];
    return {track->video_width, track->video_height};
}

bool RendererTrackRegistry::has_active_tracks() const {
    return tracks_.has_active_tracks();
}

size_t RendererTrackRegistry::count() const {
    return tracks_.count();
}

bool RendererTrackRegistry::has_preroll_blocking_track() const {
    return vr::has_preroll_blocking_track(tracks_);
}

bool RendererTrackRegistry::has_buffering_track() const {
    return vr::has_buffering_track(tracks_);
}

int RendererTrackRegistry::next_file_id_value() const {
    return next_file_id_;
}

void RendererTrackRegistry::set_next_file_id(int value) {
    next_file_id_ = value;
}

int RendererTrackRegistry::allocate_next_file_id() {
    return next_file_id_++;
}

uint64_t RendererTrackRegistry::allocate_generation() {
    return next_generation_++;
}

void RendererTrackRegistry::reset_ids() {
    next_file_id_ = 1;
    next_generation_ = 1;
}

InitialTrackOpenResult RendererTrackRegistry::open_initial_tracks(
    const std::vector<std::string>& video_paths,
    bool use_hardware_decode,
    const InitialTrackOpenHooks& hooks,
    const char* log_context) {
    return open_initial_track_pipelines(
        tracks_, video_paths, use_hardware_decode, hooks, log_context);
}

void RendererTrackRegistry::bind_to_render_sink(RenderSink& render_sink) const {
    bind_existing_tracks_to_render_sink(tracks_, render_sink);
}

void RendererTrackRegistry::stop_all(
    const RendererTrackBeforeStopCallback& before_stop) {
    tracks_.stop_all(before_stop);
}

RendererTrackAddReservation RendererTrackRegistry::reserve_add_track(
    int requested_file_id) {
    RendererTrackAddReservation reservation;
    const int slot = find_empty_slot();
    if (slot < 0) {
        reservation.failure = RendererTrackAddReservationFailure::NoEmptySlot;
        return reservation;
    }
    if (requested_file_id >= 0 &&
        find_slot_by_file_id(requested_file_id) >= 0) {
        reservation.failure =
            RendererTrackAddReservationFailure::DuplicateFileId;
        return reservation;
    }

    reservation.ok = true;
    reservation.slot = slot;
    if (requested_file_id >= 0) {
        reservation.file_id = requested_file_id;
        set_next_file_id(std::max(next_file_id_, requested_file_id + 1));
    } else {
        reservation.file_id = allocate_next_file_id();
    }
    reservation.generation = allocate_generation();
    return reservation;
}

int64_t RendererTrackRegistry::cached_duration() const {
    return cached_duration_us_;
}

void RendererTrackRegistry::set_cached_duration(int64_t duration_us) {
    cached_duration_us_ = duration_us;
}

void RendererTrackRegistry::extend_cached_duration_with(
    const TrackPipeline& track) {
    cached_duration_us_ =
        extend_track_duration_cache(cached_duration_us_, track);
}

void RendererTrackRegistry::recompute_cached_duration() {
    cached_duration_us_ = compute_track_duration_cache(tracks_);
}

int64_t RendererTrackRegistry::effective_duration_us() const {
    return resolve_effective_duration_us(tracks_, cached_duration_us_);
}

int64_t RendererTrackRegistry::offset_us_for_file_id(int file_id) const {
    const int slot = find_slot_by_file_id(file_id);
    if (slot < 0 || !tracks_[static_cast<size_t>(slot)]) {
        return 0;
    }
    return tracks_[static_cast<size_t>(slot)]->offset_us;
}

TrackAddSeekResult RendererTrackRegistry::prepare_add_track_seek_to_clock(
    TrackPipeline& track,
    int64_t current_pts_us,
    bool was_playing,
    const TrackAddSeekHooks& hooks) const {
    return vr::prepare_add_track_seek_to_clock(
        track, current_pts_us, was_playing, hooks);
}

std::vector<LayoutTrackGeometryUpdate>
RendererTrackRegistry::update_layout_track_geometry_from_decision(
    const PresentDecision& decision) {
    return vr::update_layout_track_geometry_from_decision(tracks_, decision);
}

} // namespace vr
