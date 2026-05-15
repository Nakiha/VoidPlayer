#pragma once

#include "video_renderer/layout/layout_state.h"
#include "video_renderer/track/track_pipeline.h"

#include <functional>

namespace vr {

class LayoutController {
public:
    using SlotResolver = std::function<int(int file_id)>;

    void reset(LayoutState& layout);
    void apply(LayoutState& layout,
               const LayoutState& requested,
               const SlotResolver& resolve_slot);
    LayoutState snapshot(LayoutState layout) const;
    void append_track(LayoutState& layout, int file_id, int slot);
    void append_tracks(LayoutState& layout, const TrackPipelineManager& tracks);
    void remove_track(LayoutState& layout, int file_id, const SlotResolver& resolve_slot);
    void rebuild_slot_order(LayoutState& layout, const SlotResolver& resolve_slot) const;

private:
    int file_id_order_[4] = {-1, -1, -1, -1};
};

} // namespace vr
