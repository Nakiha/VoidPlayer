#pragma once

#include "renderer/render/presentation_scheduler.h"
#include "renderer/render/render_loop_controller.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace vr {

struct PresentationLoopDriverStats {
    uint64_t tick_count = 0;
    uint64_t presentable_tick_count = 0;
    uint64_t frame_notification_count = 0;
    uint64_t deadline_sleep_count = 0;
    int64_t last_deadline_sleep_us = 0;
    int64_t last_selected_pts_us = kNoTimestampUs;
    int32_t last_present_frame_count = 0;
    bool cached_present_decision_available = false;
};

struct PresentationLoopDriverTick {
    PresentationSchedulerTick scheduler;
    std::chrono::microseconds next_sleep{0};
};

struct PresentationFrameCallbackInput {
    bool scheduler_should_notify = false;
    bool renderer_owned_target_available = false;
    bool renderer_owned_upload_succeeded = false;
};

bool should_publish_presentation_frame_callback(
    const PresentationFrameCallbackInput& input);

class PresentationLoopDriver {
public:
    void reset();

    PresentationLoopDriverTick tick(RenderSink& render_sink,
                                    bool playing,
                                    int64_t current_pts_us,
                                    double speed,
                                    std::optional<int64_t> next_event_pts_us,
                                    std::chrono::microseconds max_sleep);

    bool advance_to_clock(RenderSink& render_sink, int64_t* selected_pts_us = nullptr) const;
    PresentDecision current_present_decision(RenderSink* render_sink);
    void publish_present_decision(const PresentDecision& decision);
    void reset_presentation_state();
    void clear_cached_present_decision();

    PresentationLoopDriverStats stats() const { return stats_; }

private:
    std::chrono::microseconds compute_next_sleep(bool playing,
                                                 int64_t current_pts_us,
                                                 double speed,
                                                 std::optional<int64_t> next_event_pts_us,
                                                 std::chrono::microseconds max_sleep);

    PresentationScheduler scheduler_;
    RenderLoopController render_loop_controller_;
    PresentDecision cached_present_decision_;
    PresentationLoopDriverStats stats_;
};

} // namespace vr
