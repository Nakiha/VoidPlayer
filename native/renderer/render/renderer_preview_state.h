#pragma once

namespace vr {

// Lock contract:
// - Owns paused-preview draw/pending state.
// - Does not take locks, call callbacks, or touch track/presentation objects.
// - Callers serialize access with the renderer state lock.
class RendererPreviewState {
public:
    void reset();
    void request_redraw();
    void mark_presented(bool drawn = true);
    void clear_pending();
    bool drawn() const;
    bool pending() const;
    bool begin_draw_if_needed();
    void mark_async_pending();

private:
    bool drawn_ = false;
    bool pending_ = false;
};

} // namespace vr
