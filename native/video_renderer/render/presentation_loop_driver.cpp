#include "video_renderer/render/presentation_loop_driver.h"

namespace vr {

void PresentationLoopDriver::reset() {
    scheduler_.reset();
    render_loop_controller_.reset();
    cached_present_decision_ = {};
    stats_ = {};
}

PresentationLoopDriverTick PresentationLoopDriver::tick(
    RenderSink& render_sink,
    bool playing,
    int64_t current_pts_us,
    double speed,
    std::optional<int64_t> next_event_pts_us,
    std::chrono::microseconds max_sleep) {
    PresentationLoopDriverTick result;
    ++stats_.tick_count;

    if (!playing) {
        stats_.last_deadline_sleep_us = 0;
        return result;
    }

    result.scheduler = scheduler_.tick(render_sink);
    if (result.scheduler.has_presentable_frame) {
        cached_present_decision_ = result.scheduler.decision;
        stats_.cached_present_decision_available = true;
        ++stats_.presentable_tick_count;
        stats_.last_selected_pts_us = result.scheduler.selected_pts_us;
        stats_.last_present_frame_count = 0;
        for (const auto& frame : result.scheduler.decision.frames) {
            if (frame.has_value()) {
                ++stats_.last_present_frame_count;
            }
        }
    }
    if (result.scheduler.should_notify) {
        ++stats_.frame_notification_count;
    }

    result.next_sleep = compute_next_sleep(
        playing, current_pts_us, speed, next_event_pts_us, max_sleep);
    return result;
}

bool PresentationLoopDriver::advance_to_clock(RenderSink& render_sink,
                                              int64_t* selected_pts_us) const {
    return scheduler_.advance_to_clock(render_sink, selected_pts_us);
}

PresentDecision PresentationLoopDriver::current_present_decision(RenderSink* render_sink) {
    if (stats_.cached_present_decision_available) {
        return cached_present_decision_;
    }
    return render_sink ? render_sink->evaluate() : PresentDecision();
}

void PresentationLoopDriver::publish_present_decision(const PresentDecision& decision) {
    cached_present_decision_ = decision;
    stats_.cached_present_decision_available = decision.should_present;
    if (!decision.should_present) {
        stats_.last_present_frame_count = 0;
        return;
    }
    int64_t selected_pts_us = kNoTimestampUs;
    int32_t frame_count = 0;
    for (const auto& frame : decision.frames) {
        if (!frame.has_value()) {
            continue;
        }
        ++frame_count;
        if (selected_pts_us == kNoTimestampUs || frame->pts_us < selected_pts_us) {
            selected_pts_us = frame->pts_us;
        }
    }
    stats_.last_selected_pts_us = selected_pts_us;
    stats_.last_present_frame_count = frame_count;
}

void PresentationLoopDriver::reset_presentation_state() {
    scheduler_.reset();
    clear_cached_present_decision();
    stats_.last_selected_pts_us = kNoTimestampUs;
    stats_.last_present_frame_count = 0;
    stats_.last_deadline_sleep_us = 0;
}

void PresentationLoopDriver::clear_cached_present_decision() {
    cached_present_decision_ = {};
    stats_.cached_present_decision_available = false;
}

std::chrono::microseconds PresentationLoopDriver::compute_next_sleep(
    bool playing,
    int64_t current_pts_us,
    double speed,
    std::optional<int64_t> next_event_pts_us,
    std::chrono::microseconds max_sleep) {
    if (!playing || max_sleep.count() <= 0) {
        stats_.last_deadline_sleep_us = 0;
        return std::chrono::microseconds(0);
    }
    if (!next_event_pts_us.has_value()) {
        stats_.last_deadline_sleep_us = 1000;
        return std::chrono::milliseconds(1);
    }

    const auto sleep_for = render_loop_controller_.frame_deadline_sleep(
        current_pts_us, *next_event_pts_us, speed, max_sleep.count());
    stats_.last_deadline_sleep_us = sleep_for.count();
    if (sleep_for.count() > 0) {
        ++stats_.deadline_sleep_count;
    }
    return sleep_for;
}

} // namespace vr
