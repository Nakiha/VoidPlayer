#include "renderer/render/renderer_loop_driver.h"

namespace vr {

bool RendererLoopDriver::thread_joinable() const {
    return thread_.joinable();
}

void RendererLoopDriver::join_thread() {
    thread_.join();
}

bool RendererLoopDriver::running() const {
    return running_.load(std::memory_order_acquire);
}

void RendererLoopDriver::set_running(bool running) {
    running_.store(running, std::memory_order_release);
}

void RendererLoopDriver::reset_timing() {
    controller_.reset();
}

void RendererLoopDriver::start_loop(std::chrono::steady_clock::time_point now) {
    controller_.start(now);
}

void RendererLoopDriver::request_resize(int width, int height) {
    pending_width_.store(width);
    pending_height_.store(height);
}

std::pair<int, int> RendererLoopDriver::take_pending_resize() {
    return {pending_width_.exchange(0), pending_height_.exchange(0)};
}

void RendererLoopDriver::requeue_resize_if_empty(int width, int height) {
    int expected = 0;
    pending_width_.compare_exchange_strong(expected, width);
    expected = 0;
    pending_height_.compare_exchange_strong(expected, height);
}

RendererLoopResizeDecision RendererLoopDriver::take_resize_decision(
    std::chrono::steady_clock::time_point now) {
    RendererLoopResizeDecision decision;
    auto [width, height] = take_pending_resize();
    if (width <= 0 || height <= 0) {
        return decision;
    }
    if (controller_.should_apply_resize(now)) {
        controller_.mark_resize_applied(now);
        decision.should_apply = true;
        decision.width = width;
        decision.height = height;
        return decision;
    }
    // Too soon: re-queue so the next iteration can pick it up. Write back only
    // if no newer resize arrived in the meantime.
    requeue_resize_if_empty(width, height);
    return decision;
}

void RendererLoopDriver::clear_pending_resize() {
    pending_width_.store(0);
    pending_height_.store(0);
}

void RendererLoopDriver::reset_playback_pacing() {
    playback_pacing_.reset();
}

PlaybackPacingDecision RendererLoopDriver::evaluate_playback_pacing(
    const PlaybackPacingInput& input) {
    return playback_pacing_.evaluate(input);
}

PlaybackPacingDiagnostics
RendererLoopDriver::playback_pacing_diagnostics() const {
    return playback_pacing_.diagnostics();
}

void RendererLoopDriver::reset_preview_state() {
    preview_.reset();
}

void RendererLoopDriver::force_preview_redraw() {
    preview_.request_redraw();
}

void RendererLoopDriver::mark_preview_presented(bool drawn) {
    preview_.mark_presented(drawn);
}

void RendererLoopDriver::clear_preview_pending() {
    preview_.clear_pending();
}

bool RendererLoopDriver::preview_drawn() const {
    return preview_.drawn();
}

bool RendererLoopDriver::preview_pending() const {
    return preview_.pending();
}

bool RendererLoopDriver::begin_preview_draw_if_needed() {
    return preview_.begin_draw_if_needed();
}

void RendererLoopDriver::mark_async_preview_pending() {
    preview_.mark_async_pending();
}

RendererLoopDiagnosticDecision RendererLoopDriver::take_diagnostic_decision(
    std::chrono::steady_clock::time_point now,
    int64_t pts_us) {
    RendererLoopDiagnosticDecision decision;
    decision.should_emit =
        controller_.should_emit_diagnostics(now, pts_us, decision.pts_delta_us);
    return decision;
}

void RendererLoopDriver::reset_presentation_scheduler() {
    presentation_scheduler_.reset();
}

PresentationSchedulerTick RendererLoopDriver::tick_presentation(RenderSink& sink) {
    return presentation_scheduler_.tick(sink);
}

void RendererLoopDriver::commit_presented(
    const PresentDecision& decision) {
    presentation_scheduler_.commit_presented(decision);
}

std::chrono::microseconds RendererLoopDriver::frame_deadline_sleep(
    int64_t current_pts_us,
    int64_t next_event_pts_us,
    double speed,
    int64_t max_sleep_us) const {
    return controller_.frame_deadline_sleep(
        current_pts_us, next_event_pts_us, speed, max_sleep_us);
}

} // namespace vr
