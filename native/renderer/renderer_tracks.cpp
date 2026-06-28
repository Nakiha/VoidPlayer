#include "renderer/renderer_internal.h"

namespace vr {

bool Renderer::Impl::recreate_pipeline_for_seek(std::unique_lock<std::mutex>& state_lock,
                                          size_t slot,
                                          int64_t target_pts_us,
                                          SeekType type) {
    if (!state_lock.owns_lock()) {
        spdlog::error("[Renderer] recreate_pipeline_for_seek called without state lock");
        return false;
    }
    if (slot >= kMaxTracks || !track_controller_.has_slot(static_cast<int>(slot))) {
        return false;
    }

    const RendererTrackRecreateDetachHooks detach_hooks{
        [this](size_t detached_slot, TrackPipeline& track) {
            unregister_track_audio(track.file_id);
            render_sink_->set_track(detached_slot, nullptr);
            presentation_.reset_track(detached_slot);
        },
    };
    auto detached = track_controller_.detach_for_recreate(slot, detach_hooks);
    if (!detached.detached) {
        return false;
    }

    spdlog::info("[Renderer] Recreating pipeline for {}", detached.file_path);

    present_history_.clear_slot(slot);
    clear_present_decision_slot(external_d3d12_visible_decision_, slot);
    loop_driver_.force_preview_redraw();

    state_lock.unlock();

    track_controller_.stop_detached_pipeline(slot, detached.detached_track);

    // Give the driver a brief moment to retire the previous hardware decoder
    // objects before constructing a fresh hardware pipeline on the same file.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const SeekRequest initial_seek{target_pts_us, type};
    auto replacement = track_controller_.create_pipeline(
        detached.file_path,
        detached.use_hardware_decode,
        surface_state_.backend_kind(),
        presentation_.native_render_device(),
        &presentation_.device_mutex(),
        &initial_seek);
    if (!replacement) {
        spdlog::error("[Renderer] Failed to recreate pipeline for {}", detached.file_path);
        state_lock.lock();
        return false;
    }
    replacement->generation = detached.replacement_generation;

    const TrackPipelineStartConfig start_config{
        detached.file_id,
        detached.offset_us,
        false,
        true,
    };
    const TrackPipelineStartHooks start_hooks{
        [this](TrackPipeline& track) { configure_track_seek_callback(track); },
        [this](TrackPipeline& track) { configure_track_error_callback(track); },
        [this](TrackPipeline& track) { register_track_audio(track); },
        [this](int id) { unregister_track_audio(id); },
    };
    if (!track_controller_.configure_and_start_pipeline(
            *replacement, start_config, start_hooks, "[Renderer]")) {
        state_lock.lock();
        return false;
    }

    state_lock.lock();
    if (!initialized_ || !track_controller_.can_commit_recreated_track(slot)) {
        state_lock.unlock();
        track_controller_.stop_detached_pipeline(slot, replacement);
        state_lock.lock();
        return false;
    }

    const TrackAddCommitHooks commit_hooks{
        [this](size_t committed_slot, TrackPipeline& track) {
            render_sink_->set_track(
                committed_slot, track.track_buffer, track.file_id, track.generation);
            render_sink_->set_track_offset(committed_slot, track.offset_us);
        },
        {},
    };
    return track_controller_.commit_recreated_track(
               slot, std::move(replacement), commit_hooks) != nullptr;
}

int Renderer::Impl::add_track(const std::string& video_path,
                        bool use_hardware_decode) {
    return add_track_internal(video_path, use_hardware_decode, -1);
}

int Renderer::Impl::add_track_with_file_id(const std::string& video_path,
                                     int file_id,
                                     bool use_hardware_decode) {
    if (file_id < 0) {
        spdlog::warn("Renderer::add_track_with_file_id: invalid file_id={}", file_id);
        return -1;
    }
    return add_track_internal(video_path, use_hardware_decode, file_id);
}

int Renderer::Impl::add_track_internal(const std::string& video_path,
                                 bool use_hardware_decode,
                                 int requested_file_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    RendererTrackAddReservation reservation;
    int64_t current_pts = 0;
    TrackPlaybackMutationState playback_state;

    const TrackPlaybackMutationHooks playback_hooks{
        [this]() { timeline_.playback().pause(); },
        [this]() { timeline_.playback().play(); },
        [this](bool playing) { timeline_.set_playing(playing); },
    };

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_) return -1;

        reservation = track_controller_.reserve_add_track(requested_file_id);
        if (!reservation.ok &&
            reservation.failure == RendererTrackAddReservationFailure::NoEmptySlot) {
            spdlog::warn("Renderer::add_track: no empty slots");
            return -1;
        }
        if (!reservation.ok &&
            reservation.failure == RendererTrackAddReservationFailure::DuplicateFileId) {
            spdlog::warn("Renderer::add_track: file_id={} is already open",
                         requested_file_id);
            return -1;
        }

        playback_state = pause_playback_for_track_mutation(
            timeline_.playing(), playback_hooks);
        current_pts = timeline_.playback().clock().current_pts_us();
    }

    auto pipeline = track_controller_.create_pipeline(
        video_path,
        use_hardware_decode,
        surface_state_.backend_kind(),
        presentation_.native_render_device(),
        &presentation_.device_mutex());
    if (!pipeline) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        rollback_track_mutation_playback(playback_state, playback_hooks);
        return -1;
    }
    pipeline->generation = reservation.generation;
    const TrackPipelineStartConfig start_config{
        reservation.file_id,
        0,
        !playback_state.was_playing,
        false,
    };
    const TrackPipelineStartHooks hooks{
        [this](TrackPipeline& track) { configure_track_seek_callback(track); },
        [this](TrackPipeline& track) { configure_track_error_callback(track); },
        [this](TrackPipeline& track) { register_track_audio(track); },
        [this](int id) { unregister_track_audio(id); },
    };
    if (!track_controller_.configure_and_start_pipeline(
            *pipeline, start_config, hooks, "Renderer::add_track")) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        rollback_track_mutation_playback(playback_state, playback_hooks);
        return -1;
    }

    TrackAddSeekResult seek_result;
    int64_t track_offset_us = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_ ||
            !track_controller_.can_commit_add(static_cast<size_t>(reservation.slot))) {
            rollback_track_mutation_playback(playback_state, playback_hooks);
            return -1;
        }

        // Update duration cache
        track_controller_.extend_cached_duration_with(*pipeline);

        const TrackAddCommitHooks commit_hooks{
            [this](size_t committed_slot, TrackPipeline& track) {
                render_sink_->set_track(
                    committed_slot, track.track_buffer, track.file_id, track.generation);
                render_sink_->set_track_offset(committed_slot, track.offset_us);
            },
            [this](size_t committed_slot) {
                presentation_.reset_track(committed_slot);
            },
        };
        TrackPipeline* track = track_controller_.commit_new_track(
            static_cast<size_t>(reservation.slot), std::move(pipeline), commit_hooks);
        if (!track) {
            rollback_track_mutation_playback(playback_state, playback_hooks);
            return -1;
        }

        layout_state_.append_track(reservation.file_id, reservation.slot);

        // Seek new track to current clock position so evaluate() can find matching frames.
        // Without this, the new track starts from PTS=0 and evaluate() discards all its
        // frames as "expired" when the clock is elsewhere, causing both panels to show
        // the same old video.
        const TrackAddSeekHooks add_seek_hooks{
            [this](int file_id, bool paused) {
                if (timeline_.audio()) {
                    timeline_.audio()->set_track_decode_paused(file_id, paused);
                }
            },
        };
        seek_result = track_controller_.prepare_add_track_seek_to_clock(
            *track, current_pts, playback_state.was_playing, add_seek_hooks);
        track_offset_us = track->offset_us;

        // Force redraw, but keep already-presented frames from existing tracks so
        // they remain visible while the new track is still buffering/soft-decoding.
        loop_driver_.force_preview_redraw();
        present_history_.clear_reserved_slot(static_cast<size_t>(reservation.slot));
        clear_present_decision_slot(
            external_d3d12_visible_decision_,
            static_cast<size_t>(reservation.slot));
        loop_driver_.reset_presentation_scheduler();
    }

    if (seek_result.applied) {
        spdlog::info("Renderer::add_track: seeking slot={} to {:.3f}s (offset={:.3f}s, type={})",
                     reservation.slot,
                     seek_result.target_pts_us / 1e6,
                     track_offset_us / 1e6,
                     is_exact_seek_type(seek_result.seek_type) ? "Exact" : "Keyframe");
    }

    spdlog::info("Renderer::add_track: slot={} hw_decode={} path={}",
                 reservation.slot,
                 use_hardware_decode,
                 video_path);
    return reservation.slot;
}

void Renderer::Impl::remove_track(int file_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::unique_ptr<TrackPipeline> removed_track;
    int slot = -1;
    size_t remaining = 0;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        slot = track_controller_.find_slot_by_file_id(file_id);
        if (slot < 0) return;

        spdlog::info("Renderer::remove_track: file_id={}, slot={}", file_id, slot);

        const TrackPlaybackMutationHooks playback_hooks{
            [this]() { timeline_.playback().pause(); },
            [this]() { timeline_.playback().play(); },
            [this](bool playing) { timeline_.set_playing(playing); },
        };
        const auto playback_state = pause_playback_for_track_mutation(
            timeline_.playing(), playback_hooks);

        unregister_track_audio(file_id);
        const RendererTrackDetachHooks detach_hooks{
            [this](size_t removed_slot, TrackPipeline&) {
                render_sink_->set_track(removed_slot, nullptr);
                presentation_.reset_track(removed_slot);
            },
            [this](size_t from, size_t to, TrackPipeline& track) {
                presentation_.move_track(from, to);
                render_sink_->set_track(
                    to, track.track_buffer, track.file_id, track.generation);
                render_sink_->set_track_offset(to, track.offset_us);
                render_sink_->set_track(from, nullptr);
            },
        };
        auto detach_result =
            track_controller_.detach_and_compact_by_file_id(file_id, detach_hooks);
        if (!detach_result.removed) {
            rollback_track_mutation_playback(playback_state, playback_hooks);
            return;
        }
        removed_track = std::move(detach_result.detached_track);
        slot = detach_result.slot;
        remaining = detach_result.remaining;
        present_history_.compact_from(static_cast<size_t>(slot));
        compact_present_decision_frames(
            external_d3d12_visible_decision_,
            static_cast<size_t>(slot));

        layout_state_.remove_track(
            file_id, [this](int id) {
                return track_controller_.find_slot_by_file_id(id);
            });

        loop_driver_.force_preview_redraw();
        loop_driver_.reset_presentation_scheduler();

        finish_track_removal_playback(
            playback_state, track_controller_.first_active_slot() >= 0, playback_hooks);
    }

    if (removed_track) {
        if (removed_track->decode_thread) {
            spdlog::info("Renderer: stopping track[{}] decode ({})",
                         slot,
                         removed_track->file_path);
            removed_track->decode_thread->stop();
            spdlog::info("Renderer: track[{}] decode stopped", slot);
        }
        if (removed_track->demux_thread) {
            spdlog::info("Renderer: stopping track[{}] demux ({})",
                         slot,
                         removed_track->file_path);
            removed_track->demux_thread->stop();
            spdlog::info("Renderer: track[{}] demux stopped", slot);
        }
        removed_track.reset();
    }

    spdlog::info(
        "Renderer::remove_track: file_id={}, slot={}, remaining={}",
        file_id,
        slot,
        remaining);
}

bool Renderer::Impl::has_track(int slot) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return track_controller_.has_slot(slot);
}

std::pair<int, int> Renderer::Impl::track_dimensions(int slot) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return track_controller_.dimensions_for_slot(slot);
}

std::vector<TrackInfo> Renderer::Impl::track_infos() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return track_controller_.infos();
}

std::vector<TrackPerfStats> Renderer::Impl::track_perf_stats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return track_controller_.perf_stats(
        present_history_.snapshot(), std::chrono::steady_clock::now());
}

RendererPresentedAnchorDiagnostics
Renderer::Impl::presented_anchor_diagnostics() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return present_history_.diagnostics();
}

} // namespace vr
