#pragma once

#include "renderer/track/renderer_track_types.h"
#include "renderer/track/track_pipeline.h"
#include "renderer/track/track_pipeline_factory.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace vr {

// Owns renderer track storage, identity allocation, and cached duration state.
// Mutation and presentation components can inspect tracks through narrow accessors
// without reaching into allocation/cached-duration internals.
class RendererTrackRegistry {
public:
    std::unique_ptr<TrackPipeline> create_pipeline(
        const std::string& path,
        bool hw_decode,
        RenderBackendKind render_backend,
        void* render_device,
        std::recursive_mutex* device_mutex,
        const SeekRequest* initial_seek) const;

    TrackPipelineManager& mutable_tracks_for_mutation();
    const TrackPipelineManager& tracks_for_snapshot() const;

    void assign_missing_generations();
    int first_active_slot() const;
    int find_empty_slot() const;
    int find_slot_by_file_id(int file_id) const;
    int audio_info_slot(int preferred_file_id) const;
    int audio_sample_rate_for_slot(int slot) const;
    int audio_channels_for_slot(int slot) const;
    bool uses_hardware_codec(AVCodecID codec_id) const;
    int64_t min_current_frame_duration_us() const;
    bool has_slot(int slot) const;
    std::pair<int, int> dimensions_for_slot(int slot) const;
    bool has_active_tracks() const;
    size_t count() const;
    bool has_preroll_blocking_track() const;
    bool has_buffering_track() const;

    int next_file_id_value() const;
    void set_next_file_id(int value);
    int allocate_next_file_id();
    uint64_t allocate_generation();
    void reset_ids();

    InitialTrackOpenResult open_initial_tracks(
        const std::vector<std::string>& video_paths,
        bool use_hardware_decode,
        const InitialTrackOpenHooks& hooks,
        const char* log_context);
    void bind_to_render_sink(RenderSink& render_sink) const;
    void stop_all(const RendererTrackBeforeStopCallback& before_stop = {});

    RendererTrackAddReservation reserve_add_track(int requested_file_id);

    int64_t cached_duration() const;
    void set_cached_duration(int64_t duration_us);
    void extend_cached_duration_with(const TrackPipeline& track);
    void recompute_cached_duration();
    int64_t effective_duration_us() const;
    int64_t offset_us_for_file_id(int file_id) const;

    TrackAddSeekResult prepare_add_track_seek_to_clock(
        TrackPipeline& track,
        int64_t current_pts_us,
        bool was_playing,
        const TrackAddSeekHooks& hooks) const;
    std::vector<LayoutTrackGeometryUpdate> update_layout_track_geometry_from_decision(
        const PresentDecision& decision);

private:
    TrackPipelineFactory factory_;
    TrackPipelineManager tracks_;
    int next_file_id_ = 1;
    uint64_t next_generation_ = 1;
    int64_t cached_duration_us_ = 0;
};

} // namespace vr
