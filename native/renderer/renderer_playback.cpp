#include "renderer/renderer_internal.h"

namespace vr {

void Renderer::play() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto plan = plan_renderer_play_command(initialized_, playing_);
    if (!plan.execute) return;
    if (plan.reset_seek && seek_coordinator_) {
        seek_coordinator_->reset();
    }

    apply_playback_decode_state_locked(plan.playback_active);
    if (plan.play_clock) {
        playback_->play();
    }
    playing_ = plan.playing;
}

void Renderer::pause() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto plan = plan_renderer_pause_command();
    apply_playback_decode_state_locked(plan.playback_active);
    if (plan.pause_clock) {
        playback_->pause();
    }
    playing_ = plan.playing;
}

void Renderer::seek(int64_t target_pts_us, SeekType type, int64_t request_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (request_id >= 0) {
        pending_seek_event_request_id_ = request_id;
        pending_seek_event_target_pts_us_ = target_pts_us;
        pending_seek_event_emitted_ = false;
    }
    seek_internal(lock, target_pts_us, type);
}

void Renderer::set_loop_range(bool enabled, int64_t start_us, int64_t end_us) {
    const auto validation = validate_loop_range(enabled, start_us, end_us);
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid loop range: {}", validation.message);
        return;
    }

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto next = normalize_loop_range_state(enabled, start_us, end_us);
    if (loop_range_states_equal(loop_range_, next)) {
        return;
    }

    loop_range_ = next;
    spdlog::debug("[Renderer] loop range {}: {:.3f}s -> {:.3f}s",
                  next.enabled ? "enabled" : "disabled",
                  next.start_us / 1e6,
                  next.end_us / 1e6);
}

void Renderer::set_audible_track(int file_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!audio_coordinator_) return;
    if (file_id >= 0 && find_slot_by_file_id(file_id) < 0) {
        file_id = -1;
    }
    audio_coordinator_->set_active_track(file_id);
}

int Renderer::audible_track() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return audio_coordinator_ ? audio_coordinator_->active_track() : -1;
}

bool Renderer::has_audio() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return find_audio_info_slot_locked() >= 0;
}

int Renderer::find_audio_info_slot_locked() const {
    const int active_audio_track =
        audio_coordinator_ ? audio_coordinator_->active_track() : -1;
    if (active_audio_track >= 0) {
        const int slot = find_slot_by_file_id(active_audio_track);
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

int Renderer::audio_sample_rate() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const int slot = find_audio_info_slot_locked();
    if (slot < 0 || !tracks_[static_cast<size_t>(slot)] ||
        !tracks_[static_cast<size_t>(slot)]->demux_thread) {
        return 0;
    }
    return tracks_[static_cast<size_t>(slot)]->demux_thread->stats().sample_rate;
}

int Renderer::audio_channels() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const int slot = find_audio_info_slot_locked();
    if (slot < 0 || !tracks_[static_cast<size_t>(slot)] ||
        !tracks_[static_cast<size_t>(slot)]->demux_thread) {
        return 0;
    }
    return tracks_[static_cast<size_t>(slot)]->demux_thread->stats().channels;
}

AudioOutputStats Renderer::audio_output_stats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return audio_coordinator_ ? audio_coordinator_->stats() : AudioOutputStats{};
}

void Renderer::seek_internal(std::unique_lock<std::mutex>& state_lock,
                             int64_t target_pts_us,
                             SeekType type,
                             bool allow_deferred,
                             bool force_recreate_paused_hevc) {
    // Caller must hold lifecycle_mutex_ and state_mutex_.
    // See native/docs/SEEK_STRATEGY.md for codec/container-specific exact seek
    // limits, especially H.264 FLV streams without repeated SPS/PPS on IDR.
    if (!state_lock.owns_lock()) {
        spdlog::error("[Renderer] seek_internal called without state lock");
        return;
    }
    const PendingSeekPreviewEventState pending_event{
        pending_seek_event_request_id_ >= 0,
        pending_seek_event_emitted_,
        pending_seek_event_target_pts_us_,
    };
    const auto seek_target = resolve_seek_target(
        target_pts_us, effective_duration_us_locked(), pending_event);
    target_pts_us = seek_target.target_pts_us;
    const auto clamp_log = build_seek_clamp_log_facts(seek_target);
    if (clamp_log.should_log) {
        spdlog::info("[Renderer] seek_internal clamp: requested={:.3f}s, clamped={:.3f}s, duration={:.3f}s",
                     clamp_log.requested_seconds,
                     clamp_log.clamped_seconds,
                     clamp_log.duration_seconds);
    }
    if (seek_target.retarget_pending_event) {
        pending_seek_event_target_pts_us_ = target_pts_us;
    }
    const auto request_log = build_seek_request_log_facts(target_pts_us, type);
    spdlog::info("[Renderer] seek_internal: target={:.3f}s, type={}",
                 request_log.target_seconds,
                 request_log.type_label);
    RendererSeekClockGateInput clock_gate_input;
    clock_gate_input.allow_deferred = allow_deferred;
    clock_gate_input.playing = playing_.load();
    clock_gate_input.has_hevc_hw_track =
        allow_deferred ? has_hevc_hw_track_locked() : false;
    clock_gate_input.target_pts_us = target_pts_us;
    clock_gate_input.type = type;
    const auto clock_gate = plan_renderer_seek_clock_gate(clock_gate_input);
    if (clock_gate.seek_clock) {
        playback_->seek_clock(clock_gate.target_pts_us);
    }
    if (should_defer_paused_hevc_seek_locked(clock_gate)) {
        return;
    }

    bool applied_seek = false;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        const TrackSeekSlotApplicationHooks seek_hooks{
            TrackSeekPreparationHooks{
                [this](int file_id, bool paused) {
                    if (audio_coordinator_) {
                        audio_coordinator_->set_track_decode_paused(file_id, paused);
                    }
                },
                [this, i]() {
                    if (presentation_backend_) {
                        presentation_backend_->reset_track(i);
                    }
                },
            },
            [this, &state_lock](size_t slot, int64_t seek_target_us, SeekType seek_type) {
                return recreate_pipeline_for_seek(
                    state_lock, slot, seek_target_us, seek_type);
            },
        };
        const auto seek_result = apply_track_seek_to_slot(
            tracks_, i, target_pts_us, type, playing_.load(),
            force_recreate_paused_hevc, seek_hooks);
        if (!seek_result.slot_present) {
            continue;
        }
        const auto& seek_facts = seek_result.facts;
        if (seek_facts.warn_h264_flv_exact_seek) {
            spdlog::warn("[Renderer] Exact seek on H.264/FLV is best-effort: "
                         "streams that omit SPS/PPS on IDR frames can decode "
                         "incorrectly after seek. Remux/re-encode with repeated "
                         "headers for frame-accurate previews.");
        }
        const auto& track_target = seek_facts.target;
        const auto track_clamp_log =
            build_track_seek_target_clamp_log_facts(i, track_target);
        if (track_clamp_log.should_log) {
            spdlog::info("[Renderer] seek_internal: track[{}] target clamp "
                         "requested={:.3f}s, clamped={:.3f}s",
                         track_clamp_log.slot,
                         track_clamp_log.requested_seconds,
                         track_clamp_log.clamped_seconds);
        }
        const auto& seek_prep = seek_result.preparation;
        const auto& seek_execution = seek_result.execution;
        const auto coalescing_log = build_track_seek_coalescing_log_facts(
            i, track_target, seek_prep, seek_execution);
        if (coalescing_log.should_log) {
            spdlog::info("[Renderer] seek_internal: track[{}] coalescing HEVC HW seek during transition "
                         "(buf_state_before={}, target={:.3f}s)",
                         coalescing_log.slot,
                         coalescing_log.buffer_state_before,
                         coalescing_log.target_seconds);
        }
        if (!seek_execution.applied_seek) {
            continue;
        }
        auto* track = tracks_[i].get();
        if (!track) {
            continue;
        }
        applied_seek = true;
        const auto cleared_log = build_track_seek_cleared_log_facts(
            i,
            track_target,
            seek_prep,
            seek_execution,
            track->track_buffer->total_count());
        spdlog::info("[Renderer] seek_internal: track[{}] cleared (buf={}->{}, pq={}->0), state->Flushing, target={:.3f}s",
                     cleared_log.slot,
                     cleared_log.buffered_frames_before,
                     cleared_log.buffered_frames_after,
                     cleared_log.packet_queue_size_before,
                     cleared_log.target_seconds);
    }
    if (applied_seek) {
        preview_drawn_ = false;
        last_decision_ = PresentDecision();
        presentation_scheduler_.reset();
    }
}

bool Renderer::should_defer_paused_hevc_seek_locked(const RendererSeekClockGatePlan& gate) {
    if (!gate.evaluate_paused_hevc_defer || !seek_coordinator_) {
        return false;
    }

    const bool deferred = seek_coordinator_->should_defer_paused_hevc_seek(
        gate.playing, gate.has_hevc_hw_track, gate.target_pts_us, gate.type);
    if (deferred) {
        spdlog::info("[Renderer] Deferring paused HEVC HW seek to {:.3f}s",
                     gate.target_pts_us / 1e6);
    }
    return deferred;
}

bool Renderer::apply_deferred_paused_hevc_seek_locked(
    std::unique_lock<std::mutex>& state_lock) {
    if (!seek_coordinator_) {
        return false;
    }

    const auto deferred = seek_coordinator_->take_deferred_paused_hevc_seek(playing_.load());
    if (!deferred.has_value()) {
        return false;
    }
    spdlog::info("[Renderer] Applying deferred paused HEVC HW seek to {:.3f}s",
                 deferred->target_pts_us / 1e6);
    seek_internal(state_lock, deferred->target_pts_us, deferred->type, false, true);
    return true;
}

bool Renderer::apply_loop_range_locked(std::unique_lock<std::mutex>& state_lock) {
    const int64_t pts = playback_->clock().current_pts_us();
    LoopRangeSeekInput input;
    input.playing = playing_.load();
    input.loop_enabled = loop_range_.enabled;
    input.clock_paused = playback_->clock().is_paused();
    input.current_pts_us = pts;
    input.start_us = loop_range_.start_us;
    input.end_us = loop_range_.end_us;
    const auto decision = choose_loop_range_seek(input);
    if (!decision.should_seek) {
        return false;
    }

    spdlog::info("[Renderer] loop range boundary: pts={:.3f}s, seeking to {:.3f}s",
                 pts / 1e6, decision.target_pts_us / 1e6);
    seek_internal(state_lock, decision.target_pts_us, SeekType::Exact);
    return true;
}

void Renderer::mark_paused_hevc_seek_preview_drawn_locked() {
    if (!seek_coordinator_) {
        return;
    }
    const bool was_in_flight = seek_coordinator_->paused_hevc_seek_in_flight();
    seek_coordinator_->mark_paused_hevc_preview_drawn(has_hevc_hw_track_locked());
    if (was_in_flight && !seek_coordinator_->paused_hevc_seek_in_flight()) {
        spdlog::info("[Renderer] Paused HEVC HW seek preview ready, settle window {}ms",
                     static_cast<long long>(kPausedHevcSeekSettleDelay.count()));
    }
}

bool Renderer::has_hevc_hw_track_locked() const {
    return any_track_uses_hardware_codec(tracks_, AV_CODEC_ID_HEVC);
}

void Renderer::set_decode_paused_for_all_tracks(bool paused) {
    const TrackDecodePauseHooks hooks{
        [](size_t, TrackPipeline& track, bool paused) {
            track.decode_thread->set_decode_paused(paused);
        },
        [this](bool paused) {
            if (audio_coordinator_) {
                audio_coordinator_->set_all_decode_paused(paused);
            }
        },
    };
    apply_track_decode_pause_state(tracks_, paused, hooks);
}

void Renderer::apply_playback_decode_state_locked(bool playback_active) {
    const TrackPlaybackDecodeStateHooks hooks{
        [](size_t, TrackPipeline& track, bool enabled) {
            track.decode_thread->set_pause_after_preroll(enabled);
        },
        [](size_t, TrackPipeline& track, bool paused) {
            track.decode_thread->set_decode_paused(paused);
        },
        [this](bool paused) {
            if (audio_coordinator_) {
                audio_coordinator_->set_all_decode_paused(paused);
            }
        },
    };
    apply_track_playback_decode_state(tracks_, playback_active, hooks);
}

void Renderer::configure_track_seek_callback(TrackPipeline& track) {
    auto* dt = track.decode_thread.get();
    const int file_id = track.file_id;
    track.demux_thread->set_seek_callback(
        [this, dt, file_id](int64_t pts, SeekType type) {
            if (shutting_down_.load(std::memory_order_acquire)) {
                return;
            }
            dt->notify_seek(pts, type);
            if (audio_coordinator_) {
                audio_coordinator_->notify_seek(file_id, pts, type);
            }
        });
}

void Renderer::configure_track_error_callback(TrackPipeline& track) {
    const int file_id = track.file_id;
    const auto buffer = track.track_buffer;
    track.demux_thread->set_error_callback(
        [this, file_id, buffer](int error_code) {
            if (shutting_down_.load(std::memory_order_acquire)) {
                return;
            }
            if (buffer) {
                buffer->set_state(TrackState::Error);
            }
            RendererEvent event;
            event.type = RendererEvent::Type::TrackError;
            event.track_file_id = file_id;
            event.error_code = error_code;
            emit_event(event);
        });
}

void Renderer::register_track_audio(TrackPipeline& track) {
    if (!audio_coordinator_ ||
        !track.audio_packet_queue ||
        !track.demux_thread ||
        track.file_id < 0) {
        return;
    }
    const auto& stats = track.demux_thread->stats();
    if (!audio_coordinator_->register_track(track.file_id, *track.audio_packet_queue, stats)) {
        spdlog::warn("[Renderer] Failed to start audio decoder for file_id={}", track.file_id);
    }
}

void Renderer::unregister_track_audio(int file_id) {
    if (audio_coordinator_) {
        audio_coordinator_->unregister_track(file_id);
    }
}

int64_t Renderer::effective_duration_us_locked() const {
    return resolve_effective_duration_us(tracks_, cached_duration_us_);
}

bool Renderer::settle_eof_locked(int64_t max_presented_end_us) {
    const int64_t duration_us = effective_duration_us_locked();
    const auto decision = choose_playback_eof_settlement({
        playing_.load(),
        playback_->clock().current_pts_us(),
        max_presented_end_us,
        duration_us,
        compute_min_current_frame_duration_us(tracks_),
    });
    if (!decision.should_settle) {
        return false;
    }

    set_decode_paused_for_all_tracks(true);
    playback_->clock().seek(decision.settle_pts_us);
    playback_->clock().pause();
    playing_ = false;
    preview_drawn_ = true;
    spdlog::info("[Renderer] EOF reached: clock fixed at {:.3f}s (last_frame_end={:.3f}s, duration={:.3f}s)",
                 decision.settle_pts_us / 1e6,
                 max_presented_end_us / 1e6,
                 duration_us / 1e6);
    return true;
}

void Renderer::set_speed(double speed) {
    const auto validation = validate_playback_speed(speed);
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid playback speed: {}", validation.message);
        return;
    }

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    playback_->set_speed(speed);
}

bool Renderer::is_playing() const {
    return playing_;
}

bool Renderer::is_initialized() const {
    return initialized_;
}

int64_t Renderer::current_pts_us() const {
    return playback_->clock().current_pts_us();
}

double Renderer::current_speed() const {
    return playback_->speed();
}

size_t Renderer::track_count() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return tracks_.count();
}

int64_t Renderer::duration_us() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return cached_duration_us_;
}

void Renderer::set_track_offset(int file_id, int64_t offset_us) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    int slot = find_slot_by_file_id(file_id);
    if (slot < 0 || !tracks_[slot]) return;
    const TrackOffsetMutationHooks hooks{
        [this](size_t offset_slot, int64_t offset) {
            render_sink_->set_track_offset(offset_slot, offset);
        },
    };
    apply_track_offset_mutation(
        *tracks_[slot], static_cast<size_t>(slot), offset_us, hooks);
    preview_drawn_ = false;
}

int64_t Renderer::track_offset_us(int file_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int slot = find_slot_by_file_id(file_id);
    if (slot < 0 || !tracks_[slot]) {
        return 0;
    }
    return tracks_[slot]->offset_us;
}

} // namespace vr
