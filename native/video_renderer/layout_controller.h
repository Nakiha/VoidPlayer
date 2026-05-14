#pragma once

#include <functional>

namespace vr {

struct LayoutState;

class LayoutController {
public:
    using SlotResolver = std::function<int(int file_id)>;

    void reset(LayoutState& layout);
    void apply(LayoutState& layout,
               const LayoutState& requested,
               const SlotResolver& resolve_slot);
    LayoutState snapshot(LayoutState layout) const;
    void append_track(LayoutState& layout, int file_id, int slot);
    void remove_track(LayoutState& layout, int file_id, const SlotResolver& resolve_slot);
    void rebuild_slot_order(LayoutState& layout, const SlotResolver& resolve_slot) const;

private:
    int file_id_order_[4] = {-1, -1, -1, -1};
};

} // namespace vr
