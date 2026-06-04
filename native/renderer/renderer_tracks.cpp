#include "renderer/renderer_internal.h"

namespace vr {

void Renderer::assign_missing_track_generations_locked() {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i] && tracks_[i]->generation == 0) {
            tracks_[i]->generation = next_track_generation_++;
        }
    }
}

int Renderer::first_active_track() const {
    return tracks_.first_active_slot();
}

int Renderer::find_slot_by_file_id(int file_id) const {
    return tracks_.find_slot_by_file_id(file_id);
}

int Renderer::find_empty_slot() const {
    return tracks_.find_empty_slot();
}

std::unique_ptr<TrackPipeline> Renderer::create_pipeline(
    const std::string& path,
    bool hw_decode,
    const SeekRequest* initial_seek) {
    TrackPipelineOpenOptions options;
    options.render_backend = render_backend_kind_;
    return track_pipeline_factory_.create_opened_pipeline(
        path, hw_decode, initial_seek, options);
}

bool Renderer::recreate_pipeline_for_seek(std::unique_lock<std::mutex>& state_lock,
                                          size_t slot,
                                          int64_t target_pts_us,
                                          SeekType type) {
    if (!state_lock.owns_lock()) {
        spdlog::error("[Renderer] recreate_pipeline_for_seek called without state lock");
        return false;
    }
    if (slot >= kMaxTracks || !tracks_[slot]) {
        return false;
    }

    auto current = std::move(tracks_[slot]);
    const auto file_path = current->file_path;
    const auto file_id = current->file_id;
    const auto offset_us = current->offset_us;
    const auto use_hardware_decode = current->use_hardware_decode;
    const auto generation = next_track_generation_++;

    spdlog::info("[Renderer] Recreating pipeline for {}", file_path);

    unregister_track_audio(file_id);
    render_sink_->set_track(slot, nullptr);
    if (presentation_backend_) {
        presentation_backend_->reset_track(slot);
    }
    clear_present_decision_slot(last_decision_, slot);
    preview_drawn_ = false;

    state_lock.unlock();

    stop_detached_track_pipeline(slot, current);

    // Give the driver a brief moment to retire the previous D3D11VA decoder
    // objects before constructing a fresh hardware pipeline on the same file.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const SeekRequest initial_seek{target_pts_us, type};
    auto replacement = create_pipeline(file_path, use_hardware_decode, &initial_seek);
    if (!replacement) {
        spdlog::error("[Renderer] Failed to recreate pipeline for {}", file_path);
        state_lock.lock();
        return false;
    }
    replacement->generation = generation;

    const TrackPipelineStartConfig start_config{
        file_id,
        offset_us,
        false,
        true,
    };
    const TrackPipelineStartHooks start_hooks{
        [this](TrackPipeline& track) { configure_track_seek_callback(track); },
        [this](TrackPipeline& track) { configure_track_error_callback(track); },
        [this](TrackPipeline& track) { register_track_audio(track); },
        [this](int id) { unregister_track_audio(id); },
    };
    if (!configure_and_start_track_pipeline(
            *replacement, start_config, start_hooks, "[Renderer]")) {
        state_lock.lock();
        return false;
    }

    state_lock.lock();
    if (!initialized_ || tracks_[slot]) {
        state_lock.unlock();
        stop_detached_track_pipeline(slot, replacement);
        state_lock.lock();
        return false;
    }

    render_sink_->set_track(slot, replacement->track_buffer, file_id, generation);
    render_sink_->set_track_offset(slot, offset_us);
    tracks_[slot] = std::move(replacement);
    return true;
}

int Renderer::add_track(const std::string& video_path,
                        bool use_hardware_decode) {
    return add_track_internal(video_path, use_hardware_decode, -1);
}

int Renderer::add_track_with_file_id(const std::string& video_path,
                                     int file_id,
                                     bool use_hardware_decode) {
    if (file_id < 0) {
        spdlog::warn("Renderer::add_track_with_file_id: invalid file_id={}", file_id);
        return -1;
    }
    return add_track_internal(video_path, use_hardware_decode, file_id);
}

int Renderer::add_track_internal(const std::string& video_path,
                                 bool use_hardware_decode,
                                 int requested_file_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    int slot = -1;
    int new_file_id = 0;
    uint64_t new_generation = 0;
    int64_t current_pts = 0;
    TrackPlaybackMutationState playback_state;

    const TrackPlaybackMutationHooks playback_hooks{
        [this]() { playback_->pause(); },
        [this]() { playback_->play(); },
        [this](bool playing) { playing_ = playing; },
    };

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_) return -1;

        slot = find_empty_slot();
        if (slot < 0) {
            spdlog::warn("Renderer::add_track: no empty slots");
            return -1;
        }
        if (requested_file_id >= 0 && find_slot_by_file_id(requested_file_id) >= 0) {
            spdlog::warn("Renderer::add_track: file_id={} is already open",
                         requested_file_id);
            return -1;
        }

        playback_state = pause_playback_for_track_mutation(
            playing_.load(), playback_hooks);
        if (requested_file_id >= 0) {
            new_file_id = requested_file_id;
            next_file_id_ = std::max(next_file_id_, requested_file_id + 1);
        } else {
            new_file_id = next_file_id_++;
        }
        new_generation = next_track_generation_++;
        current_pts = playback_->clock().current_pts_us();
    }

    auto pipeline = create_pipeline(video_path, use_hardware_decode);
    if (!pipeline) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        rollback_track_mutation_playback(playback_state, playback_hooks);
        return -1;
    }
    pipeline->generation = new_generation;
    const TrackPipelineStartConfig start_config{
        new_file_id,
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
    if (!configure_and_start_track_pipeline(
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
            slot < 0 ||
            slot >= static_cast<int>(kMaxTracks) ||
            tracks_[static_cast<size_t>(slot)]) {
            rollback_track_mutation_playback(playback_state, playback_hooks);
            return -1;
        }

        // Update duration cache
        cached_duration_us_ = extend_track_duration_cache(cached_duration_us_, *pipeline);

        const TrackAddCommitHooks commit_hooks{
            [this](size_t committed_slot, TrackPipeline& track) {
                render_sink_->set_track(
                    committed_slot, track.track_buffer, track.file_id, track.generation);
                render_sink_->set_track_offset(committed_slot, track.offset_us);
            },
            [this](size_t committed_slot) {
                if (presentation_backend_) {
                    presentation_backend_->reset_track(committed_slot);
                }
            },
        };
        TrackPipeline* track = commit_new_track_pipeline(
            tracks_, static_cast<size_t>(slot), std::move(pipeline), commit_hooks);
        if (!track) {
            rollback_track_mutation_playback(playback_state, playback_hooks);
            return -1;
        }

        layout_controller_.append_track(layout_, new_file_id, slot);

        // Seek new track to current clock position so evaluate() can find matching frames.
        // Without this, the new track starts from PTS=0 and evaluate() discards all its
        // frames as "expired" when the clock is elsewhere, causing both panels to show
        // the same old video.
        const TrackAddSeekHooks add_seek_hooks{
            [this](int file_id, bool paused) {
                if (audio_coordinator_) {
                    audio_coordinator_->set_track_decode_paused(file_id, paused);
                }
            },
        };
        seek_result = prepare_add_track_seek_to_clock(
            *track, current_pts, playback_state.was_playing, add_seek_hooks);
        track_offset_us = track->offset_us;

        // Force redraw, but keep already-presented frames from existing tracks so
        // they remain visible while the new track is still buffering/soft-decoding.
        preview_drawn_ = false;
        last_decision_.frames[slot] = std::nullopt;
        last_decision_.file_ids[slot] = -1;
        last_decision_.track_generations[slot] = 0;
        presentation_scheduler_.reset();
    }

    if (seek_result.applied) {
        spdlog::info("Renderer::add_track: seeking slot={} to {:.3f}s (offset={:.3f}s, type={})",
                     slot,
                     seek_result.target_pts_us / 1e6,
                     track_offset_us / 1e6,
                     is_exact_seek_type(seek_result.seek_type) ? "Exact" : "Keyframe");
    }

    spdlog::info("Renderer::add_track: slot={} hw_decode={} path={}",
                 slot,
                 use_hardware_decode,
                 video_path);
    return slot;
}

void Renderer::remove_track(int file_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::unique_ptr<TrackPipeline> removed_track;
    int slot = -1;
    size_t remaining = 0;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        slot = find_slot_by_file_id(file_id);
        if (slot < 0) return;

        spdlog::info("Renderer::remove_track: file_id={}, slot={}", file_id, slot);

        const TrackPlaybackMutationHooks playback_hooks{
            [this]() { playback_->pause(); },
            [this]() { playback_->play(); },
            [this](bool playing) { playing_ = playing; },
        };
        const auto playback_state = pause_playback_for_track_mutation(
            playing_.load(), playback_hooks);

        unregister_track_audio(file_id);
        render_sink_->set_track(static_cast<size_t>(slot), nullptr);
        if (presentation_backend_) {
            presentation_backend_->reset_track(static_cast<size_t>(slot));
        }
        removed_track = std::move(tracks_[static_cast<size_t>(slot)]);
        if (removed_track && removed_track->demux_thread) {
            removed_track->demux_thread->set_seek_callback({});
            removed_track->demux_thread->set_error_callback({});
        }

        tracks_.compact_from(static_cast<size_t>(slot), [this](
            size_t from,
            size_t to,
            TrackPipeline& track) {
            if (presentation_backend_) {
                presentation_backend_->move_track(from, to);
            }
            render_sink_->set_track(to, track.track_buffer, track.file_id, track.generation);
            render_sink_->set_track_offset(to, track.offset_us);
            render_sink_->set_track(from, nullptr);
        });
        compact_present_decision_frames(last_decision_, static_cast<size_t>(slot));

        layout_controller_.remove_track(
            layout_, file_id, [this](int id) { return find_slot_by_file_id(id); });

        cached_duration_us_ = compute_track_duration_cache(tracks_);
        preview_drawn_ = false;
        presentation_scheduler_.reset();
        remaining = tracks_.count();

        finish_track_removal_playback(
            playback_state, first_active_track() >= 0, playback_hooks);
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

bool Renderer::has_track(int slot) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks)) return false;
    return tracks_[slot] != nullptr;
}

std::pair<int, int> Renderer::track_dimensions(int slot) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks) || !tracks_[slot]) {
        return {0, 0};
    }
    return {tracks_[slot]->video_width, tracks_[slot]->video_height};
}

std::vector<TrackInfo> Renderer::track_infos() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return snapshot_track_infos(tracks_);
}

std::vector<TrackPerfStats> Renderer::track_perf_stats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto now = std::chrono::steady_clock::now();
    const double elapsed_s = perf_baseline_tracker_.elapsed_seconds(now);
    const bool should_rotate = perf_baseline_tracker_.should_rotate(elapsed_s);
    PresentDecision filtered_last_decision = last_decision_;
    filter_present_decision_against_tracks(filtered_last_decision, tracks_);
    const auto snapshot = snapshot_track_perf_stats_collection(
        tracks_, filtered_last_decision, perf_baseline_tracker_, elapsed_s);

    if (should_rotate) {
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!snapshot.frames_decoded_by_slot[i].has_value()) {
                continue;
            }
            perf_baseline_tracker_.update_baseline_frames(
                i, *snapshot.frames_decoded_by_slot[i]);
        }
    }

    // Reset shared timer once after all tracks are processed
    if (should_rotate) {
        perf_baseline_tracker_.rotate_timer(now);
    }
    return snapshot.stats;
}

} // namespace vr
