#include "renderer/renderer_internal.h"

namespace vr {

void Renderer::Impl::play() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto plan = plan_renderer_play_command(initialized_, timeline_.playing());
    if (!plan.execute) return;
    if (plan.reset_seek && timeline_.seek()) {
        timeline_.seek()->reset();
    }

    apply_playback_decode_state_locked(plan.playback_active);
    if (plan.play_clock) {
        timeline_.playback().play();
    }
    timeline_.set_playing(plan.playing);
}

void Renderer::Impl::pause() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto plan = plan_renderer_pause_command();
    apply_playback_decode_state_locked(plan.playback_active);
    if (plan.pause_clock) {
        timeline_.playback().pause();
    }
    timeline_.set_playing(plan.playing);
}

void Renderer::Impl::seek(int64_t target_pts_us, SeekType type, int64_t request_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (request_id >= 0) {
        timeline_.begin_pending_seek_preview_event(request_id, target_pts_us);
    }
    seek_internal(lock, target_pts_us, type);
}

void Renderer::Impl::set_loop_range(bool enabled, int64_t start_us, int64_t end_us) {
    const auto validation = validate_loop_range(enabled, start_us, end_us);
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid loop range: {}", validation.message);
        return;
    }

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto next = normalize_loop_range_state(enabled, start_us, end_us);
    if (loop_range_states_equal(timeline_.loop_range(), next)) {
        return;
    }

    timeline_.set_loop_range(next);
    spdlog::debug("[Renderer] loop range {}: {:.3f}s -> {:.3f}s",
                  next.enabled ? "enabled" : "disabled",
                  next.start_us / 1e6,
                  next.end_us / 1e6);
}

void Renderer::Impl::set_audible_track(int file_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!timeline_.audio()) return;
    if (file_id >= 0 && track_controller_.find_slot_by_file_id(file_id) < 0) {
        file_id = -1;
    }
    timeline_.audio()->set_active_track(file_id);
}

int Renderer::Impl::audible_track() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return timeline_.audio() ? timeline_.audio()->active_track() : -1;
}

bool Renderer::Impl::has_audio() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const int active_audio_track =
        timeline_.audio() ? timeline_.audio()->active_track() : -1;
    return track_controller_.audio_info_slot(active_audio_track) >= 0;
}

int Renderer::Impl::audio_sample_rate() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const int active_audio_track =
        timeline_.audio() ? timeline_.audio()->active_track() : -1;
    const int slot = track_controller_.audio_info_slot(active_audio_track);
    return track_controller_.audio_sample_rate_for_slot(slot);
}

int Renderer::Impl::audio_channels() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const int active_audio_track =
        timeline_.audio() ? timeline_.audio()->active_track() : -1;
    const int slot = track_controller_.audio_info_slot(active_audio_track);
    return track_controller_.audio_channels_for_slot(slot);
}

AudioOutputStats Renderer::Impl::audio_output_stats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return timeline_.audio() ? timeline_.audio()->stats() : AudioOutputStats{};
}

void Renderer::Impl::seek_internal(std::unique_lock<std::mutex>& state_lock,
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
    const bool playing_snapshot = timeline_.playing();
    const auto seek_preparation = timeline_.prepare_seek(
        target_pts_us,
        type,
        track_controller_.effective_duration_us(),
        allow_deferred,
        playing_snapshot,
        allow_deferred ? has_hevc_hw_track_locked() : false);
    target_pts_us = seek_preparation.target.target_pts_us;
    const auto clamp_log = build_seek_clamp_log_facts(seek_preparation.target);
    if (clamp_log.should_log) {
        spdlog::info("[Renderer] seek_internal clamp: requested={:.3f}s, clamped={:.3f}s, duration={:.3f}s",
                     clamp_log.requested_seconds,
                     clamp_log.clamped_seconds,
                     clamp_log.duration_seconds);
    }
    const auto request_log = build_seek_request_log_facts(target_pts_us, type);
    spdlog::info("[Renderer] seek_internal: target={:.3f}s, type={}",
                 request_log.target_seconds,
                 request_log.type_label);
    if (seek_preparation.clock_gate.deferred) {
        spdlog::info("[Renderer] Deferring paused HEVC HW seek to {:.3f}s",
                     seek_preparation.clock_gate.plan.target_pts_us / 1e6);
        return;
    }

    const RendererTrackSeekHooks seek_hooks{
        [this](int file_id, bool paused) {
            if (timeline_.audio()) {
                timeline_.audio()->set_track_decode_paused(file_id, paused);
            }
        },
        [this](size_t slot) {
            presentation_.reset_track(slot);
        },
        [this, &state_lock](size_t slot, int64_t seek_target_us, SeekType seek_type) {
            return recreate_pipeline_for_seek(
                state_lock, slot, seek_target_us, seek_type);
        },
    };
    const bool applied_seek = track_controller_.apply_seek_to_all_and_log(
        target_pts_us,
        type,
        playing_snapshot,
        force_recreate_paused_hevc,
        seek_hooks);
    if (applied_seek) {
        loop_driver_.force_preview_redraw();
        present_history_.reset();
        loop_driver_.reset_presentation_scheduler();
    }
}

bool Renderer::Impl::apply_deferred_paused_hevc_seek_locked(
    std::unique_lock<std::mutex>& state_lock) {
    const auto deferred =
        timeline_.take_deferred_paused_hevc_seek(timeline_.playing());
    if (!deferred.has_value()) {
        return false;
    }
    spdlog::info("[Renderer] Applying deferred paused HEVC HW seek to {:.3f}s",
                 deferred->target_pts_us / 1e6);
    seek_internal(state_lock, deferred->target_pts_us, deferred->type, false, true);
    return true;
}

bool Renderer::Impl::apply_loop_range_locked(std::unique_lock<std::mutex>& state_lock) {
    const auto decision = timeline_.evaluate_loop_range_seek(timeline_.playing());
    if (!decision.should_seek) {
        return false;
    }

    spdlog::info("[Renderer] loop range boundary: pts={:.3f}s, seeking to {:.3f}s",
                 decision.current_pts_us / 1e6, decision.target_pts_us / 1e6);
    seek_internal(state_lock, decision.target_pts_us, SeekType::Exact);
    return true;
}

void Renderer::Impl::mark_paused_hevc_seek_preview_drawn_locked() {
    const auto result =
        timeline_.mark_paused_hevc_preview_drawn(has_hevc_hw_track_locked());
    if (result.was_in_flight && !result.in_flight) {
        spdlog::info("[Renderer] Paused HEVC HW seek preview ready, settle window {}ms",
                     static_cast<long long>(kPausedHevcSeekSettleDelay.count()));
    }
}

bool Renderer::Impl::has_hevc_hw_track_locked() const {
    return track_controller_.uses_hardware_codec(AV_CODEC_ID_HEVC);
}

void Renderer::Impl::set_decode_paused_for_all_tracks(bool paused) {
    const TrackDecodePauseHooks hooks{
        [](size_t, TrackPipeline& track, bool paused) {
            track.decode_thread->set_decode_paused(paused);
        },
        [this](bool paused) {
            if (timeline_.audio()) {
                timeline_.audio()->set_all_decode_paused(paused);
            }
        },
    };
    track_controller_.set_decode_paused_for_all(paused, hooks);
}

void Renderer::Impl::apply_playback_decode_state_locked(bool playback_active) {
    const TrackPlaybackDecodeStateHooks hooks{
        [](size_t, TrackPipeline& track, bool enabled) {
            track.decode_thread->set_pause_after_preroll(enabled);
        },
        [](size_t, TrackPipeline& track, bool paused) {
            track.decode_thread->set_decode_paused(paused);
        },
        [this](bool paused) {
            if (timeline_.audio()) {
                timeline_.audio()->set_all_decode_paused(paused);
            }
        },
    };
    track_controller_.apply_playback_decode_state(playback_active, hooks);
}

void Renderer::Impl::configure_track_seek_callback(TrackPipeline& track) {
    auto* dt = track.decode_thread.get();
    const int file_id = track.file_id;
    track.demux_thread->set_seek_callback(
        [this, dt, file_id](int64_t pts, SeekType type) {
            if (shutting_down_.load(std::memory_order_acquire)) {
                return;
            }
            dt->notify_seek(pts, type);
            if (timeline_.audio()) {
                timeline_.audio()->notify_seek(file_id, pts, type);
            }
        });
}

void Renderer::Impl::configure_track_error_callback(TrackPipeline& track) {
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

void Renderer::Impl::register_track_audio(TrackPipeline& track) {
    if (!timeline_.audio() ||
        !track.audio_packet_queue ||
        !track.demux_thread ||
        track.file_id < 0) {
        return;
    }
    const auto& stats = track.demux_thread->stats();
    if (!timeline_.audio()->register_track(track.file_id, *track.audio_packet_queue, stats)) {
        spdlog::warn("[Renderer] Failed to start audio decoder for file_id={}", track.file_id);
    }
}

void Renderer::Impl::unregister_track_audio(int file_id) {
    if (timeline_.audio()) {
        timeline_.audio()->unregister_track(file_id);
    }
}

bool Renderer::Impl::settle_eof_locked(int64_t max_presented_end_us) {
    const int64_t duration_us = track_controller_.effective_duration_us();
    const auto decision = choose_playback_eof_settlement({
        timeline_.playing(),
        timeline_.playback().clock().current_pts_us(),
        max_presented_end_us,
        duration_us,
        track_controller_.min_current_frame_duration_us(),
    });
    if (!decision.should_settle) {
        return false;
    }

    set_decode_paused_for_all_tracks(true);
    timeline_.playback().clock().seek(decision.settle_pts_us);
    timeline_.playback().clock().pause();
    timeline_.set_playing(false);
    loop_driver_.mark_preview_presented(true);
    spdlog::info("[Renderer] EOF reached: clock fixed at {:.3f}s (last_frame_end={:.3f}s, duration={:.3f}s)",
                 decision.settle_pts_us / 1e6,
                 max_presented_end_us / 1e6,
                 duration_us / 1e6);
    return true;
}

void Renderer::Impl::set_speed(double speed) {
    const auto validation = validate_playback_speed(speed);
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid playback speed: {}", validation.message);
        return;
    }

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    timeline_.playback().set_speed(speed);
}

bool Renderer::Impl::is_playing() const {
    return timeline_.playing();
}

bool Renderer::Impl::is_initialized() const {
    return initialized_;
}

int64_t Renderer::Impl::current_pts_us() const {
    return timeline_.playback().clock().current_pts_us();
}

double Renderer::Impl::current_speed() const {
    return timeline_.playback().speed();
}

size_t Renderer::Impl::track_count() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return track_controller_.count();
}

int64_t Renderer::Impl::duration_us() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return track_controller_.cached_duration_us();
}

void Renderer::Impl::set_track_offset(int file_id, int64_t offset_us) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    const TrackOffsetMutationHooks hooks{
        [this](size_t offset_slot, int64_t offset) {
            render_sink_->set_track_offset(offset_slot, offset);
        },
    };
    if (!track_controller_.apply_track_offset(file_id, offset_us, hooks)) {
        return;
    }
    loop_driver_.force_preview_redraw();
}

int64_t Renderer::Impl::track_offset_us(int file_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return track_controller_.offset_us_for_file_id(file_id);
}

} // namespace vr
