#include "renderer/layout/renderer_layout_state.h"

#include <algorithm>

namespace vr {

void RendererLayoutState::reset() {
    controller_.reset(layout_);
}

void RendererLayoutState::reset_revisions() {
    intent_revision_.store(0, std::memory_order_relaxed);
    revision_ = 0;
    last_presented_revision_ = 0;
    clear_pending();
}

void RendererLayoutState::append_track(int file_id, int slot) {
    controller_.append_track(layout_, file_id, slot);
}

void RendererLayoutState::remove_track(int file_id, const SlotResolver& resolve_slot) {
    controller_.remove_track(layout_, file_id, resolve_slot);
}

void RendererLayoutState::apply_current(const LayoutState& state,
                                        uint64_t revision,
                                        const SlotResolver& resolve_slot) {
    controller_.apply(layout_, state, resolve_slot);
    revision_ = std::max(revision_ + 1, revision);
}

uint64_t RendererLayoutState::next_intent_revision() {
    return intent_revision_.fetch_add(1, std::memory_order_relaxed) + 1;
}

void RendererLayoutState::set_pending(LayoutState state, uint64_t revision) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_ = std::move(state);
    pending_revision_ = revision;
}

bool RendererLayoutState::consume_pending_if_newer(const SlotResolver& resolve_slot) {
    std::optional<LayoutState> pending;
    uint64_t pending_revision = 0;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (!pending_.has_value()) {
            return false;
        }
        pending = pending_;
        pending_revision = pending_revision_;
        pending_.reset();
        pending_revision_ = 0;
    }
    if (pending_revision <= revision_) {
        return false;
    }
    apply_current(*pending, pending_revision, resolve_slot);
    return true;
}

void RendererLayoutState::clear_pending() {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.reset();
    pending_revision_ = 0;
}

LayoutState RendererLayoutState::public_snapshot() const {
    {
        std::lock_guard<std::mutex> pending_lock(pending_mutex_);
        if (pending_.has_value() && pending_revision_ > revision_) {
            return *pending_;
        }
    }
    return controller_.snapshot(layout_);
}

LayoutState RendererLayoutState::current_for_draw() const {
    return layout_;
}

uint64_t RendererLayoutState::current_revision() const {
    return revision_;
}

uint64_t RendererLayoutState::last_presented_revision() const {
    return last_presented_revision_;
}

uint64_t RendererLayoutState::latest_requested_revision() const {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    return std::max(revision_, pending_revision_);
}

bool RendererLayoutState::mark_presented_if_newer(uint64_t revision) {
    if (revision <= last_presented_revision_) {
        return false;
    }
    last_presented_revision_ = revision;
    return true;
}

bool RendererLayoutState::is_stale(uint64_t revision) const {
    return revision != revision_;
}

RendererLayoutPresentationCommit RendererLayoutState::commit_presented_draw(
    uint64_t revision) {
    RendererLayoutPresentationCommit result;
    result.stale = is_stale(revision);
    result.current_revision = revision_;
    result.marked_presented = mark_presented_if_newer(revision);
    return result;
}

void RendererLayoutState::adjust_view_offset_for_resize(
    int old_width,
    int old_height,
    int new_width,
    int new_height,
    const LayoutTrackGeometryList& tracks) {
    adjust_layout_view_offset_for_resize(
        layout_, old_width, old_height, new_width, new_height, tracks);
}

void RendererLayoutState::set_background_color(float r, float g, float b, float a) {
    background_color_[0] = std::clamp(r, 0.0f, 1.0f);
    background_color_[1] = std::clamp(g, 0.0f, 1.0f);
    background_color_[2] = std::clamp(b, 0.0f, 1.0f);
    background_color_[3] = std::clamp(a, 0.0f, 1.0f);
}

void RendererLayoutState::copy_background_color(float out[4]) const {
    for (int i = 0; i < 4; ++i) {
        out[i] = background_color_[i];
    }
}

} // namespace vr
