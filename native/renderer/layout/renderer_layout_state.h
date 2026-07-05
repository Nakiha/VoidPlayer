#pragma once

#include "renderer/layout/layout_controller.h"
#include "renderer/layout/layout_geometry.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace vr {

struct RendererLayoutPresentationCommit {
    bool stale = false;
    bool marked_presented = false;
    uint64_t current_revision = 0;
};

// Lock contract:
// - Owns layout state, pending-layout mutex, layout revisions, background color,
//   and viewport-compositor activity deadline.
// - Current layout/revision methods are called while the owner holds
//   state_mutex_; pending-layout methods lock only the internal pending mutex.
// - Does not include or retain renderer owner objects and does not touch
//   playback, track pipelines beyond small layout helpers, or presentation backends.
class RendererLayoutState {
public:
    using SlotResolver = LayoutController::SlotResolver;

    void reset();
    void reset_revisions();
    void append_track(int file_id, int slot);
    void remove_track(int file_id, const SlotResolver& resolve_slot);

    void apply_current(const LayoutState& state,
                       uint64_t revision,
                       const SlotResolver& resolve_slot);
    uint64_t next_intent_revision();
    void set_pending(LayoutState state, uint64_t revision);
    bool consume_pending_if_newer(const SlotResolver& resolve_slot);
    void clear_pending();
    LayoutState public_snapshot() const;

    LayoutState current_for_draw() const;
    uint64_t current_revision() const;
    uint64_t last_presented_revision() const;
    uint64_t latest_requested_revision() const;
    bool mark_presented_if_newer(uint64_t revision);
    bool is_stale(uint64_t revision) const;
    RendererLayoutPresentationCommit commit_presented_draw(uint64_t revision);

    void adjust_view_offset_for_resize(int old_width,
                                       int old_height,
                                       int new_width,
                                       int new_height,
                                       const LayoutTrackGeometryList& tracks);
    void set_background_color(float r, float g, float b, float a);
    void copy_background_color(float out[4]) const;

    void note_viewport_compositor_activity(int64_t active_until_us);
    void set_viewport_compositor_active(bool active);
    bool viewport_compositor_persistent_active() const;
    bool viewport_compositor_active(int64_t now_us) const;

private:
    LayoutController controller_;
    LayoutState layout_;
    std::atomic<uint64_t> intent_revision_{0};
    mutable std::mutex pending_mutex_;
    std::optional<LayoutState> pending_;
    uint64_t pending_revision_ = 0;
    uint64_t revision_ = 0;
    uint64_t last_presented_revision_ = 0;
    float background_color_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    std::atomic<int64_t> viewport_compositor_active_until_us_{0};
    std::atomic<bool> viewport_compositor_active_{false};
};

} // namespace vr
