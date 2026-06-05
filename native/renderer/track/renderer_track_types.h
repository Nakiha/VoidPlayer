#pragma once

#include "renderer/track/track_lifecycle.h"
#include "renderer/track/track_step_policy.h"
#include "renderer/render/renderer_draw_snapshot.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vr {

enum class RendererTrackAddReservationFailure {
    None,
    NoEmptySlot,
    DuplicateFileId,
};

struct RendererTrackAddReservation {
    bool ok = false;
    int slot = -1;
    int file_id = 0;
    uint64_t generation = 0;
    RendererTrackAddReservationFailure failure =
        RendererTrackAddReservationFailure::None;
};

struct RendererTrackDetachHooks {
    std::function<void(size_t slot, TrackPipeline& track)> clear_slot;
    std::function<void(size_t from, size_t to, TrackPipeline& track)> move_slot;
};

struct RendererTrackDetachResult {
    bool removed = false;
    int slot = -1;
    size_t remaining = 0;
    std::unique_ptr<TrackPipeline> detached_track;
};

struct RendererTrackRecreateDetachHooks {
    std::function<void(size_t slot, TrackPipeline& track)> clear_slot;
};

struct RendererTrackRecreateDetachResult {
    bool detached = false;
    size_t slot = 0;
    std::string file_path;
    int file_id = 0;
    int64_t offset_us = 0;
    bool use_hardware_decode = true;
    uint64_t replacement_generation = 0;
    std::unique_ptr<TrackPipeline> detached_track;
};

struct RendererPausedCachedDecision {
    PresentDecision decision;
    std::optional<int64_t> first_pts_us;
    bool has_frame = false;
};

struct RendererPausedLayoutDecision {
    PresentDecision decision;
    size_t active_track_count = 0;
    bool has_frame = false;
};

struct RendererPausedRefreshDecision {
    PresentDecision decision;
    bool has_frame = false;
};

struct RendererTrackReferenceSnapshot {
    int slot = -1;
    int64_t offset_us = 0;
};

struct RendererLayoutTrackReference {
    int file_id = -1;
    int slot = -1;
};

struct RendererTrackSeekHooks {
    std::function<void(int file_id, bool paused)> set_audio_decode_paused;
    std::function<void(size_t slot)> reset_presenter_track;
    std::function<bool(size_t slot, int64_t target_pts_us, SeekType type)>
        recreate_pipeline_for_seek;
};

struct RendererTrackSeekApplicationResult {
    size_t slot = 0;
    TrackSeekSlotApplicationResult seek;
    size_t buffered_frames_after = 0;
};

using RendererTrackBeforeStopCallback =
    std::function<void(size_t slot, TrackPipeline& track)>;

} // namespace vr
